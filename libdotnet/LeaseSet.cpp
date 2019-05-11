#include <string.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <zlib.h> // for crc32
#include "DotNetEndian.h"
#include "Crypto.h"
#include "Ed25519.h"
#include "Log.h"
#include "Timestamp.h"
#include "NetDb.hpp"
#include "Tunnel.h"
#include "LeaseSet.h"

namespace dotnet
{
namespace data
{

	LeaseSet::LeaseSet (bool storeLeases):
		m_IsValid (false), m_StoreLeases (storeLeases), m_ExpirationTime (0), m_EncryptionKey (nullptr), 
		m_Buffer (nullptr), m_BufferLen (0)
	{
	}

	LeaseSet::LeaseSet (const uint8_t * buf, size_t len, bool storeLeases):
		m_IsValid (true), m_StoreLeases (storeLeases), m_ExpirationTime (0), m_EncryptionKey (nullptr)
	{
		m_Buffer = new uint8_t[len];
		memcpy (m_Buffer, buf, len);
		m_BufferLen = len;
		ReadFromBuffer ();
	}

	void LeaseSet::Update (const uint8_t * buf, size_t len, bool verifySignature)
	{
		if (len > m_BufferLen)
		{
			auto oldBuffer = m_Buffer;
			m_Buffer = new uint8_t[len];
			delete[] oldBuffer;
		}
		memcpy (m_Buffer, buf, len);
		m_BufferLen = len;
		ReadFromBuffer (false, verifySignature);
	}

	void LeaseSet::PopulateLeases ()
	{
		m_StoreLeases = true;
		ReadFromBuffer (false);
	}

	void LeaseSet::ReadFromBuffer (bool readIdentity, bool verifySignature)
	{
		if (readIdentity || !m_Identity)
			m_Identity = std::make_shared<IdentityEx>(m_Buffer, m_BufferLen);
		size_t size = m_Identity->GetFullLen ();
		if (size > m_BufferLen)
		{
			LogPrint (eLogError, "LeaseSet: identity length ", size, " exceeds buffer size ", m_BufferLen);
			m_IsValid = false;
			return;
		}
		if (m_StoreLeases)
		{
			if (!m_EncryptionKey) m_EncryptionKey = new uint8_t[256];
			memcpy (m_EncryptionKey, m_Buffer + size, 256);
		}	
		size += 256; // encryption key
		size += m_Identity->GetSigningPublicKeyLen (); // unused signing key
		uint8_t num = m_Buffer[size];
		size++; // num
		LogPrint (eLogDebug, "LeaseSet: read num=", (int)num);
		if (!num || num > MAX_NUM_LEASES)
		{
			LogPrint (eLogError, "LeaseSet: incorrect number of leases", (int)num);
			m_IsValid = false;
			return;
		}

		UpdateLeasesBegin ();

		// process leases
		m_ExpirationTime = 0;
		auto ts = dotnet::util::GetMillisecondsSinceEpoch ();
		const uint8_t * leases = m_Buffer + size;
		for (int i = 0; i < num; i++)
		{
			Lease lease;
			lease.tunnelGateway = leases;
			leases += 32; // gateway
			lease.tunnelID = bufbe32toh (leases);
			leases += 4; // tunnel ID
			lease.endDate = bufbe64toh (leases);
			leases += 8; // end date
			UpdateLease (lease, ts);
		}
		if (!m_ExpirationTime)
		{
			LogPrint (eLogWarning, "LeaseSet: all leases are expired. Dropped");
			m_IsValid = false;
			return;
		}
		m_ExpirationTime += LEASE_ENDDATE_THRESHOLD;

		UpdateLeasesEnd ();

		// verify
		if (verifySignature && !m_Identity->Verify (m_Buffer, leases - m_Buffer, leases))
		{
			LogPrint (eLogWarning, "LeaseSet: verification failed");
			m_IsValid = false;
		}
	}

	void LeaseSet::UpdateLeasesBegin ()
	{
		// reset existing leases
		if (m_StoreLeases)
			for (auto& it: m_Leases)
				it->isUpdated = false;
		else
			m_Leases.clear ();
	}

	void LeaseSet::UpdateLeasesEnd ()
	{
		// delete old leases
		if (m_StoreLeases)
		{
			for (auto it = m_Leases.begin (); it != m_Leases.end ();)
			{
				if (!(*it)->isUpdated)
				{
					(*it)->endDate = 0; // somebody might still hold it
					m_Leases.erase (it++);
				}
				else
					++it;
			}
		}
	}

	void LeaseSet::UpdateLease (const Lease& lease, uint64_t ts)
	{
		if (ts < lease.endDate + LEASE_ENDDATE_THRESHOLD)
		{
			if (lease.endDate > m_ExpirationTime)
				m_ExpirationTime = lease.endDate;
			if (m_StoreLeases)
			{
				auto ret = m_Leases.insert (std::make_shared<Lease>(lease));
				if (!ret.second) (*ret.first)->endDate = lease.endDate; // update existing
				(*ret.first)->isUpdated = true;
				// check if lease's gateway is in our netDb
				if (!netdb.FindRouter (lease.tunnelGateway))
				{
					// if not found request it
					LogPrint (eLogInfo, "LeaseSet: Lease's tunnel gateway not found, requesting");
					netdb.RequestDestination (lease.tunnelGateway);
				}
			}
		}
		else
			LogPrint (eLogWarning, "LeaseSet: Lease is expired already ");
	}

	uint64_t LeaseSet::ExtractTimestamp (const uint8_t * buf, size_t len) const
	{
		if (!m_Identity) return 0;
		size_t size = m_Identity->GetFullLen ();
		if (size > len) return 0;
		size += 256; // encryption key
		size += m_Identity->GetSigningPublicKeyLen (); // unused signing key
		if (size > len) return 0;
		uint8_t num = buf[size];
		size++; // num
		if (size + num*LEASE_SIZE > len) return 0;
		uint64_t timestamp= 0 ;
		for (int i = 0; i < num; i++)
		{
			size += 36; // gateway (32) + tunnelId(4)
			auto endDate = bufbe64toh (buf + size);
			size += 8; // end date
			if (!timestamp || endDate < timestamp)
				timestamp = endDate;
		}
		return timestamp;
	}

	bool LeaseSet::IsNewer (const uint8_t * buf, size_t len) const
	{
		return ExtractTimestamp (buf, len) > ExtractTimestamp (m_Buffer, m_BufferLen);
	}

	bool LeaseSet::ExpiresSoon(const uint64_t dlt, const uint64_t fudge) const
	{
		auto now = dotnet::util::GetMillisecondsSinceEpoch ();
		if (fudge) now += rand() % fudge;
		if (now >= m_ExpirationTime) return true;
		return	m_ExpirationTime - now <= dlt;
	}

  const std::vector<std::shared_ptr<const Lease> > LeaseSet::GetNonExpiredLeases (bool withThreshold) const
  {
    return GetNonExpiredLeasesExcluding( [] (const Lease & l) -> bool { return false; }, withThreshold);
  }

	const std::vector<std::shared_ptr<const Lease> > LeaseSet::GetNonExpiredLeasesExcluding (LeaseInspectFunc exclude, bool withThreshold) const
	{
		auto ts = dotnet::util::GetMillisecondsSinceEpoch ();
		std::vector<std::shared_ptr<const Lease> > leases;
		for (const auto& it: m_Leases)
		{
			auto endDate = it->endDate;
			if (withThreshold)
				endDate += LEASE_ENDDATE_THRESHOLD;
			else
				endDate -= LEASE_ENDDATE_THRESHOLD;
			if (ts < endDate && !exclude(*it))
				leases.push_back (it);
		}
		return leases;
	}

	bool LeaseSet::HasExpiredLeases () const
	{
		auto ts = dotnet::util::GetMillisecondsSinceEpoch ();
		for (const auto& it: m_Leases)
			if (ts >= it->endDate) return true;
		return false;
	}

	bool LeaseSet::IsExpired () const
	{
		if (m_StoreLeases && IsEmpty ()) return true;
		auto ts = dotnet::util::GetMillisecondsSinceEpoch ();
		return ts > m_ExpirationTime;
	}

	void LeaseSet::Encrypt (const uint8_t * data, uint8_t * encrypted, BN_CTX * ctx) const
	{
		if (!m_EncryptionKey) return;
		auto encryptor = m_Identity->CreateEncryptor (m_EncryptionKey);
		if (encryptor)
			encryptor->Encrypt (data, encrypted, ctx, true);
	}

	void LeaseSet::SetBuffer (const uint8_t * buf, size_t len)
	{
		if (m_Buffer) delete[] m_Buffer;
		m_Buffer = new uint8_t[len];
		m_BufferLen = len;
		memcpy (m_Buffer, buf, len);
	}

	BlindedPublicKey::BlindedPublicKey (std::shared_ptr<const IdentityEx> identity, SigningKeyType blindedKeyType):
		m_BlindedSigType (blindedKeyType)
	{
		if (!identity) return;
		auto len = identity->GetSigningPublicKeyLen ();
		m_PublicKey.resize (len);
		memcpy (m_PublicKey.data (), identity->GetSigningPublicKeyBuffer (), len);
		m_SigType = identity->GetSigningKeyType ();
	}

	BlindedPublicKey::BlindedPublicKey (const std::string& b33)
	{
		uint8_t addr[40]; // TODO: define length from b33
		size_t l = dotnet::data::Base32ToByteStream (b33.c_str (), b33.length (), addr, 40);
		uint32_t checksum = crc32 (0, addr + 3, l - 3); 
		// checksum is Little Endian
		addr[0] ^= checksum; addr[1] ^= (checksum >> 8); addr[2] ^= (checksum >> 16);  
		uint8_t flag = addr[0];
		size_t offset = 1;	
		if (flag & 0x01) // two bytes signatures
		{
			m_SigType = bufbe16toh (addr + offset); offset += 2;
			m_BlindedSigType = bufbe16toh (addr + offset); offset += 2;
		}
		else // one byte sig
		{
			m_SigType = addr[offset]; offset++;
			m_BlindedSigType = addr[offset]; offset++;
		}
		std::unique_ptr<dotnet::crypto::Verifier> blindedVerifier (dotnet::data::IdentityEx::CreateVerifier (m_SigType));
		if (blindedVerifier)
		{
			auto len = blindedVerifier->GetPublicKeyLen ();
			if (offset + len <= l)
			{
				m_PublicKey.resize (len);
				memcpy (m_PublicKey.data (), addr + offset, len);
			}
			else
				LogPrint (eLogError, "LeaseSet2: public key in b33 address is too short for signature type ", (int)m_SigType);	
		}
		else
			LogPrint (eLogError, "LeaseSet2: unknown signature type ", (int)m_SigType, " in b33");
	}

	std::string BlindedPublicKey::ToB33 () const
	{
		if (m_PublicKey.size () > 32) return ""; // assume 25519
		uint8_t addr[35]; char str[60]; // TODO: define actual length
		addr[0] = 0; // flags
		addr[1] = m_SigType; // sig type
		addr[2] = m_BlindedSigType; // blinded sig type
		memcpy (addr + 3, m_PublicKey.data (), m_PublicKey.size ());
		uint32_t checksum = crc32 (0, addr + 3, m_PublicKey.size ()); 
		// checksum is Little Endian
		addr[0] ^= checksum; addr[1] ^= (checksum >> 8); addr[2] ^= (checksum >> 16); 
		auto l = ByteStreamToBase32 (addr, m_PublicKey.size () + 3, str, 60);
		return std::string (str, str + l);
	}

	void BlindedPublicKey::GetCredential (uint8_t * credential) const
	{
		// A = destination's signing public key 
		// stA = signature type of A, 2 bytes big endian
		uint16_t stA = htobe16 (GetSigType ());
		// stA1 = signature type of blinded A, 2 bytes big endian
		uint16_t stA1 = htobe16 (GetBlindedSigType ());	
		// credential = H("credential", A || stA || stA1)
		H ("credential", { {GetPublicKey (), GetPublicKeyLen ()}, {(const uint8_t *)&stA, 2}, {(const uint8_t *)&stA1, 2} }, credential);
	}

	void BlindedPublicKey::GetSubcredential (const uint8_t * blinded, size_t len, uint8_t * subcredential) const
	{
		uint8_t credential[32];
		GetCredential (credential);
		// subcredential = H("subcredential", credential || blindedPublicKey)
		H ("subcredential", { {credential, 32}, {blinded, len} }, subcredential);
	}

	void BlindedPublicKey::GenerateAlpha (const char * date, uint8_t * seed) const
	{
		uint16_t stA = htobe16 (GetSigType ()), stA1 = htobe16 (GetBlindedSigType ());
		uint8_t salt[32];
		//seed = HKDF(H("DOTNETGenerateAlpha", keydata), datestring || secret, "dotnetblinding1", 64)	
		H ("DOTNETGenerateAlpha", { {GetPublicKey (), GetPublicKeyLen ()}, {(const uint8_t *)&stA, 2}, {(const uint8_t *)&stA1, 2} }, salt);
		dotnet::crypto::HKDF (salt, (const uint8_t *)date, 8, "dotnetblinding1", seed);
	}

	void BlindedPublicKey::GetBlindedKey (const char * date, uint8_t * blindedKey) const
	{
		uint8_t seed[64];	
		GenerateAlpha (date, seed);	
		dotnet::crypto::GetEd25519 ()->BlindPublicKey (GetPublicKey (), seed, blindedKey);
	}

	void BlindedPublicKey::BlindPrivateKey (const uint8_t * priv, const char * date, uint8_t * blindedPriv, uint8_t * blindedPub) const
	{
		uint8_t seed[64];	
		GenerateAlpha (date, seed);	
		dotnet::crypto::GetEd25519 ()->BlindPrivateKey (priv, seed, blindedPriv, blindedPub);
	}

	void BlindedPublicKey::H (const std::string& p, const std::vector<std::pair<const uint8_t *, size_t> >& bufs, uint8_t * hash) const 
	{
		SHA256_CTX ctx;
		SHA256_Init (&ctx);
		SHA256_Update (&ctx, p.c_str (), p.length ());
		for (const auto& it: bufs)	
			SHA256_Update (&ctx, it.first, it.second);
		SHA256_Final (hash, &ctx);
	}

	dotnet::data::IdentHash BlindedPublicKey::GetStoreHash (const char * date) const
	{
		dotnet::data::IdentHash hash;
		if (m_BlindedSigType == dotnet::data::SIGNING_KEY_TYPE_REDDSA_SHA512_ED25519 ||
			m_BlindedSigType == SIGNING_KEY_TYPE_EDDSA_SHA512_ED25519)
		{
			uint8_t blinded[32];
			if (date)
				GetBlindedKey (date, blinded);
			else
			{
				char currentDate[9];
				dotnet::util::GetCurrentDate (currentDate);
				GetBlindedKey (currentDate, blinded);	
			}	
			auto stA1 = htobe16 (m_BlindedSigType);
			SHA256_CTX ctx;
			SHA256_Init (&ctx);
			SHA256_Update (&ctx, (const uint8_t *)&stA1, 2);
			SHA256_Update (&ctx, blinded, 32);
			SHA256_Final ((uint8_t *)hash, &ctx);
		}
		else
			LogPrint (eLogError, "LeaseSet2: blinded key type ", (int)m_BlindedSigType, " is not supported");			
		return hash;
	}

	LeaseSet2::LeaseSet2 (uint8_t storeType, const uint8_t * buf, size_t len, bool storeLeases):
		LeaseSet (storeLeases), m_StoreType (storeType), m_OrigStoreType (storeType)
	{	
		SetBuffer (buf, len);
		if (storeType == NETDB_STORE_TYPE_ENCRYPTED_LEASESET2)
			ReadFromBufferEncrypted (buf, len, nullptr);
		else
			ReadFromBuffer (buf, len);
	}

	LeaseSet2::LeaseSet2 (const uint8_t * buf, size_t len, std::shared_ptr<const BlindedPublicKey> key):
		LeaseSet (true), m_StoreType (NETDB_STORE_TYPE_ENCRYPTED_LEASESET2), m_OrigStoreType (NETDB_STORE_TYPE_ENCRYPTED_LEASESET2)
	{
		ReadFromBufferEncrypted (buf, len, key);
	}

	void LeaseSet2::Update (const uint8_t * buf, size_t len, bool verifySignature)
	{	
		SetBuffer (buf, len);
		if (GetStoreType () != NETDB_STORE_TYPE_ENCRYPTED_LEASESET2)
			ReadFromBuffer (buf, len, false, verifySignature);	
		// TODO: implement encrypted
	}
		
	void LeaseSet2::ReadFromBuffer (const uint8_t * buf, size_t len, bool readIdentity, bool verifySignature)
	{
		// standard LS2 header
		std::shared_ptr<const IdentityEx> identity;
		if (readIdentity)
		{	
			identity = std::make_shared<IdentityEx>(buf, len);
			SetIdentity (identity);
		}
		else
			identity = GetIdentity ();
		size_t offset = identity->GetFullLen ();
		if (offset + 8 >= len) return;
		m_PublishedTimestamp = bufbe32toh (buf + offset); offset += 4; // published timestamp (seconds)
		uint16_t expires = bufbe16toh (buf + offset); offset += 2; // expires (seconds)
		SetExpirationTime ((m_PublishedTimestamp + expires)*1000LL); // in milliseconds
		uint16_t flags = bufbe16toh (buf + offset); offset += 2; // flags
		if (flags & LEASESET2_FLAG_OFFLINE_KEYS)
		{
			// transient key
			m_TransientVerifier = ProcessOfflineSignature (identity, buf, len, offset);
			if (!m_TransientVerifier)
			{ 
				LogPrint (eLogError, "LeaseSet2: offline signature failed");
				return;
			}
		}
		// type specific part
		size_t s = 0;
		switch (m_StoreType)
		{
			case NETDB_STORE_TYPE_STANDARD_LEASESET2:
				s = ReadStandardLS2TypeSpecificPart (buf + offset, len - offset);
			break;
			case NETDB_STORE_TYPE_META_LEASESET2:
				s = ReadMetaLS2TypeSpecificPart (buf + offset, len - offset);
			break;
			default:
				LogPrint (eLogWarning, "LeaseSet2: Unexpected store type ", (int)m_StoreType);
		}
		if (!s) return;
		offset += s;
		if (verifySignature || m_TransientVerifier)
		{	
			// verify signature
			bool verified = m_TransientVerifier ? VerifySignature (m_TransientVerifier, buf, len, offset) :
				VerifySignature (identity, buf, len, offset);	
			SetIsValid (verified);	
		}
	}

	template<typename Verifier>
	bool LeaseSet2::VerifySignature (Verifier& verifier, const uint8_t * buf, size_t len, size_t signatureOffset)
	{
		if (signatureOffset + verifier->GetSignatureLen () > len) return false;
		// we assume buf inside DatabaseStore message, so buf[-1] is valid memory
		// change it for signature verification, and restore back	
		uint8_t c = buf[-1];
		const_cast<uint8_t *>(buf)[-1] = m_StoreType;
		bool verified = verifier->Verify (buf - 1, signatureOffset + 1, buf + signatureOffset); 
		const_cast<uint8_t *>(buf)[-1] = c;
		if (!verified)
			LogPrint (eLogWarning, "LeaseSet2: verification failed");
		return verified;
	}

	size_t LeaseSet2::ReadStandardLS2TypeSpecificPart (const uint8_t * buf, size_t len)
	{
		size_t offset = 0;
		// properties
		uint16_t propertiesLen = bufbe16toh (buf + offset); offset += 2; 
		offset += propertiesLen; // skip for now. TODO: implement properties
		if (offset + 1 >= len) return 0;
		// key sections
		uint16_t currentKeyType = 0;
		int numKeySections = buf[offset]; offset++;
		for (int i = 0; i < numKeySections; i++)
		{
			uint16_t keyType = bufbe16toh (buf + offset); offset += 2; // encryption key type
			if (offset + 2 >= len) return 0;
			uint16_t encryptionKeyLen = bufbe16toh (buf + offset); offset += 2; 
			if (offset + encryptionKeyLen >= len) return 0;
			if (IsStoreLeases ()) // create encryptor with leases only
			{
				// we pick first valid key, higher key type has higher priority 4-1-0
				// if two keys with of the same type, pick first
				auto encryptor = dotnet::data::IdentityEx::CreateEncryptor (keyType, buf + offset);
				if (encryptor && (!m_Encryptor || keyType > currentKeyType))
				{
					m_Encryptor = encryptor; // TODO: atomic
					currentKeyType = keyType;
				}
			}
			offset += encryptionKeyLen; 
		}	
		// leases
		if (offset + 1 >= len) return 0;	
		int numLeases = buf[offset]; offset++;
		auto ts = dotnet::util::GetMillisecondsSinceEpoch ();
		if (IsStoreLeases ())
		{
			UpdateLeasesBegin ();
			for (int i = 0; i < numLeases; i++)
			{
				if (offset + LEASE2_SIZE > len) return 0;
				Lease lease;
				lease.tunnelGateway = buf + offset; offset += 32; // gateway
				lease.tunnelID = bufbe32toh (buf + offset); offset += 4; // tunnel ID
				lease.endDate = bufbe32toh (buf + offset)*1000LL; offset += 4; // end date
				UpdateLease (lease, ts);
			}
			UpdateLeasesEnd ();
		}
		else
			offset += numLeases*LEASE2_SIZE; // 40 bytes per lease
		return offset;
	}

	size_t LeaseSet2::ReadMetaLS2TypeSpecificPart (const uint8_t * buf, size_t len)
	{
		size_t offset = 0;
		// properties
		uint16_t propertiesLen = bufbe16toh (buf + offset); offset += 2; 
		offset += propertiesLen; // skip for now. TODO: implement properties
		// entries			
		if (offset + 1 >= len) return 0;
		int numEntries = buf[offset]; offset++;
		for (int i = 0; i < numEntries; i++)
		{
			if (offset + 40 >= len) return 0;
			offset += 32; // hash
			offset += 3; // flags
 			offset += 1; // cost
			offset += 4; // expires
		}
		// revocations
		if (offset + 1 >= len) return 0;
		int numRevocations = buf[offset]; offset++;	
		for (int i = 0; i < numRevocations; i++)
		{
			if (offset + 32 > len) return 0;
			offset += 32; // hash
		}
		return offset;
	}

	void LeaseSet2::ReadFromBufferEncrypted (const uint8_t * buf, size_t len, std::shared_ptr<const BlindedPublicKey> key)
	{
		size_t offset = 0;
		// blinded key
		if (len < 2) return;
		const uint8_t * stA1 = buf + offset; // stA1 = blinded signature type, 2 bytes big endian
		uint16_t blindedKeyType = bufbe16toh (stA1); offset += 2;
		std::unique_ptr<dotnet::crypto::Verifier> blindedVerifier (dotnet::data::IdentityEx::CreateVerifier (blindedKeyType));
		if (!blindedVerifier) return;
		auto blindedKeyLen = blindedVerifier->GetPublicKeyLen ();			
		if (offset + blindedKeyLen >= len) return;
		const uint8_t * blindedPublicKey = buf + offset;
		blindedVerifier->SetPublicKey (blindedPublicKey); offset += blindedKeyLen;
		// expiration
		if (offset + 8 >= len) return;
		const uint8_t * publishedTimestamp = buf + offset;
		m_PublishedTimestamp = bufbe32toh (publishedTimestamp); offset += 4; // published timestamp (seconds)
		uint16_t expires = bufbe16toh (buf + offset); offset += 2; // expires (seconds)
		SetExpirationTime ((m_PublishedTimestamp + expires)*1000LL); // in milliseconds
		uint16_t flags = bufbe16toh (buf + offset); offset += 2; // flags
		if (flags & LEASESET2_FLAG_OFFLINE_KEYS)
		{
			// transient key
			m_TransientVerifier = ProcessOfflineSignature (blindedVerifier, buf, len, offset);
			if (!m_TransientVerifier) 
			{
				LogPrint (eLogError, "LeaseSet2: offline signature failed");
				return;
			}
		}
		// outer ciphertext
		if (offset + 2 > len) return;
		uint16_t lenOuterCiphertext = bufbe16toh (buf + offset); offset += 2;
		const uint8_t * outerCiphertext = buf + offset;	
		offset += lenOuterCiphertext;		
		// verify signature
		bool verified = m_TransientVerifier ? VerifySignature (m_TransientVerifier, buf, len, offset) :
			VerifySignature (blindedVerifier, buf, len, offset);	
		SetIsValid (verified);
		// handle ciphertext
		if (verified && key && lenOuterCiphertext >= 32)
		{
			SetIsValid (false); // we must verify it again in Layer 2 
			if (blindedKeyType == dotnet::data::SIGNING_KEY_TYPE_REDDSA_SHA512_ED25519)
			{
				// verify blinding
				char date[9];
				dotnet::util::GetDateString (m_PublishedTimestamp, date);
				uint8_t blinded[32];
				key->GetBlindedKey (date, blinded);
				if (memcmp (blindedPublicKey, blinded, 32))
				{
					LogPrint (eLogError, "LeaseSet2: blinded public key doesn't match");
					return;
				}	
			}	
			// outer key
			// outerInput = subcredential || publishedTimestamp
			uint8_t subcredential[36];
			key->GetSubcredential (blindedPublicKey, blindedKeyLen, subcredential);
			memcpy (subcredential + 32, publishedTimestamp, 4);
			// outerSalt = outerCiphertext[0:32]
			// keys = HKDF(outerSalt, outerInput, "ELS2_L1K", 44)
			uint8_t keys[64]; // 44 bytes actual data
			dotnet::crypto::HKDF (outerCiphertext, subcredential, 36, "ELS2_L1K", keys);
			// decrypt Layer 1
			// outerKey = keys[0:31]
			// outerIV = keys[32:43]
			size_t lenOuterPlaintext = lenOuterCiphertext - 32;
			std::vector<uint8_t> outerPlainText (lenOuterPlaintext);
			dotnet::crypto::ChaCha20 (outerCiphertext + 32, lenOuterPlaintext, keys, keys + 32, outerPlainText.data ());
			// inner key
			// innerInput = authCookie || subcredential || publishedTimestamp, TODO: non-empty authCookie
			// innerSalt = innerCiphertext[0:32]
			// keys = HKDF(innerSalt, innerInput, "ELS2_L2K", 44)
			// skip 1 byte flags
			dotnet::crypto::HKDF (outerPlainText.data () + 1, subcredential, 36, "ELS2_L2K", keys); // no authCookie
			// decrypt Layer 2
			// innerKey = keys[0:31]
			// innerIV = keys[32:43]
			size_t lenInnerPlaintext = lenOuterPlaintext - 32 - 1;
			std::vector<uint8_t> innerPlainText (lenInnerPlaintext);
			dotnet::crypto::ChaCha20 (outerPlainText.data () + 32 + 1, lenInnerPlaintext, keys, keys + 32, innerPlainText.data ());
			if (innerPlainText[0] == NETDB_STORE_TYPE_STANDARD_LEASESET2 || innerPlainText[0] == NETDB_STORE_TYPE_META_LEASESET2)
			{
				// override store type and buffer
				m_StoreType = innerPlainText[0]; 
				SetBuffer (innerPlainText.data () + 1, lenInnerPlaintext - 1);
				// parse and verify Layer 2
				ReadFromBuffer (innerPlainText.data () + 1, lenInnerPlaintext - 1);
			}
			else
				LogPrint (eLogError, "LeaseSet2: unexpected LeaseSet type ", (int)innerPlainText[0], " inside encrypted LeaseSet");
		}	
	}

	void LeaseSet2::Encrypt (const uint8_t * data, uint8_t * encrypted, BN_CTX * ctx) const
	{
		auto encryptor = m_Encryptor; // TODO: atomic
		if (encryptor)
			encryptor->Encrypt (data, encrypted, ctx, true);	
	}

	uint64_t LeaseSet2::ExtractTimestamp (const uint8_t * buf, size_t len) const
	{
		if (len < 8) return 0;	
		if (m_StoreType == NETDB_STORE_TYPE_ENCRYPTED_LEASESET2)
		{
			// encrypted LS2
			size_t offset = 0;
			uint16_t blindedKeyType = bufbe16toh (buf + offset); offset += 2;
			std::unique_ptr<dotnet::crypto::Verifier> blindedVerifier (dotnet::data::IdentityEx::CreateVerifier (blindedKeyType));
			if (!blindedVerifier) return 0 ;
			auto blindedKeyLen = blindedVerifier->GetPublicKeyLen ();			
			if (offset + blindedKeyLen + 6 >= len) return 0;
			offset += blindedKeyLen;
			uint32_t timestamp = bufbe32toh (buf + offset); offset += 4; 
			uint16_t expires = bufbe16toh (buf + offset); offset += 2; 
			return (timestamp + expires)* 1000LL;
		}
		else
		{
			auto identity = GetIdentity ();
			if (!identity) return 0;
			size_t offset = identity->GetFullLen ();
			if (offset + 6 >= len) return 0;
			uint32_t timestamp = bufbe32toh (buf + offset); offset += 4; 
			uint16_t expires = bufbe16toh (buf + offset); offset += 2; 
			return (timestamp + expires)* 1000LL;
		}
	}

	LocalLeaseSet::LocalLeaseSet (std::shared_ptr<const IdentityEx> identity, const uint8_t * encryptionPublicKey, std::vector<std::shared_ptr<dotnet::tunnel::InboundTunnel> > tunnels):
		m_ExpirationTime (0), m_Identity (identity)
	{
		int num = tunnels.size ();
		if (num > MAX_NUM_LEASES) num = MAX_NUM_LEASES;
		// identity
		auto signingKeyLen = m_Identity->GetSigningPublicKeyLen ();
		m_BufferLen = m_Identity->GetFullLen () + 256 + signingKeyLen + 1 + num*LEASE_SIZE + m_Identity->GetSignatureLen ();
		m_Buffer = new uint8_t[m_BufferLen];
		auto offset = m_Identity->ToBuffer (m_Buffer, m_BufferLen);
		memcpy (m_Buffer + offset, encryptionPublicKey, 256);
		offset += 256;
		memset (m_Buffer + offset, 0, signingKeyLen);
		offset += signingKeyLen;
		// num leases
		m_Buffer[offset] = num;
		offset++;
		// leases
		m_Leases = m_Buffer + offset;
		auto currentTime = dotnet::util::GetMillisecondsSinceEpoch ();
		for (int i = 0; i < num; i++)
		{
			memcpy (m_Buffer + offset, tunnels[i]->GetNextIdentHash (), 32);
			offset += 32; // gateway id
			htobe32buf (m_Buffer + offset, tunnels[i]->GetNextTunnelID ());
			offset += 4; // tunnel id
			uint64_t ts = tunnels[i]->GetCreationTime () + dotnet::tunnel::TUNNEL_EXPIRATION_TIMEOUT - dotnet::tunnel::TUNNEL_EXPIRATION_THRESHOLD; // 1 minute before expiration
			ts *= 1000; // in milliseconds
			if (ts > m_ExpirationTime) m_ExpirationTime = ts;
			// make sure leaseset is newer than previous, but adding some time to expiration date
			ts += (currentTime - tunnels[i]->GetCreationTime ()*1000LL)*2/dotnet::tunnel::TUNNEL_EXPIRATION_TIMEOUT; // up to 2 secs
			htobe64buf (m_Buffer + offset, ts);
			offset += 8; // end date
		}
		//  we don't sign it yet. must be signed later on
	}

	LocalLeaseSet::LocalLeaseSet (std::shared_ptr<const IdentityEx> identity, const uint8_t * buf, size_t len):
		m_ExpirationTime (0), m_Identity (identity)
	{
		if (buf)
		{
			m_BufferLen = len;
			m_Buffer = new uint8_t[m_BufferLen];
			memcpy (m_Buffer, buf, len);
		}
		else
		{
			m_Buffer = nullptr;
			m_BufferLen = 0;
		}
	}

	bool LocalLeaseSet::IsExpired () const
	{
		auto ts = dotnet::util::GetMillisecondsSinceEpoch ();
		return ts > m_ExpirationTime;
	}

	bool LeaseSetBufferValidate(const uint8_t * ptr, size_t sz, uint64_t & expires)
	{
		IdentityEx ident(ptr, sz);
		size_t size = ident.GetFullLen ();
		if (size > sz)
		{
			LogPrint (eLogError, "LeaseSet: identity length ", size, " exceeds buffer size ", sz);
			return false;
		}
		// encryption key
		size += 256;
		// signing key (unused)
		size += ident.GetSigningPublicKeyLen ();
		uint8_t numLeases = ptr[size];
		++size;
		if (!numLeases || numLeases > MAX_NUM_LEASES)
		{
			LogPrint (eLogError, "LeaseSet: incorrect number of leases", (int)numLeases);
			return false;
		}
		const uint8_t * leases = ptr + size;
		expires = 0;
		/** find lease with the max expiration timestamp */
		for (int i = 0; i < numLeases; i++)
		{
			leases += 36; // gateway + tunnel ID
			uint64_t endDate = bufbe64toh (leases);
			leases += 8; // end date
			if(endDate > expires)
				expires = endDate;
		}
		return ident.Verify(ptr, leases - ptr, leases);
	}

	LocalLeaseSet2::LocalLeaseSet2 (uint8_t storeType, const dotnet::data::PrivateKeys& keys, 
		uint16_t keyType, uint16_t keyLen, const uint8_t * encryptionPublicKey, 
		std::vector<std::shared_ptr<dotnet::tunnel::InboundTunnel> > tunnels):
		LocalLeaseSet (keys.GetPublic (), nullptr, 0)
	{
		auto identity = keys.GetPublic ();
		// assume standard LS2 
		int num = tunnels.size ();
		if (num > MAX_NUM_LEASES) num = MAX_NUM_LEASES;
		m_BufferLen = identity->GetFullLen () + 4/*published*/ + 2/*expires*/ + 2/*flag*/ + 2/*properties len*/ +
			1/*num keys*/ + 2/*key type*/ + 2/*key len*/ + keyLen/*key*/ + 1/*num leases*/ + num*LEASE2_SIZE + keys.GetSignatureLen ();
		uint16_t flags = 0;
		if (keys.IsOfflineSignature ()) 
		{
			flags |= LEASESET2_FLAG_OFFLINE_KEYS;
			m_BufferLen += keys.GetOfflineSignature ().size ();	
		}

		m_Buffer = new uint8_t[m_BufferLen + 1];
		m_Buffer[0] = storeType;	
		// LS2 header
		auto offset = identity->ToBuffer (m_Buffer + 1, m_BufferLen) + 1;
		auto timestamp = dotnet::util::GetSecondsSinceEpoch ();
		htobe32buf (m_Buffer + offset, timestamp); offset += 4; // published timestamp (seconds)
		uint8_t * expiresBuf = m_Buffer + offset; offset += 2; // expires, fill later
		htobe16buf (m_Buffer + offset, flags); offset += 2; // flags
		if (keys.IsOfflineSignature ())
		{
			// offline signature
			const auto& offlineSignature = keys.GetOfflineSignature ();
			memcpy (m_Buffer + offset, offlineSignature.data (), offlineSignature.size ());
			offset += offlineSignature.size ();
		}
		htobe16buf (m_Buffer + offset, 0); offset += 2; // properties len
		// keys	
		m_Buffer[offset] = 1; offset++; // 1 key
		htobe16buf (m_Buffer + offset, keyType); offset += 2; // key type 
		htobe16buf (m_Buffer + offset, keyLen); offset += 2; // key len 	
		memcpy (m_Buffer + offset, encryptionPublicKey, keyLen); offset += keyLen; // key
		// leases
		uint32_t expirationTime = 0; // in seconds
		m_Buffer[offset] = num; offset++; // num leases
		for (int i = 0; i < num; i++)
		{
			memcpy (m_Buffer + offset, tunnels[i]->GetNextIdentHash (), 32);
			offset += 32; // gateway id
			htobe32buf (m_Buffer + offset, tunnels[i]->GetNextTunnelID ());
			offset += 4; // tunnel id
			auto ts = tunnels[i]->GetCreationTime () + dotnet::tunnel::TUNNEL_EXPIRATION_TIMEOUT - dotnet::tunnel::TUNNEL_EXPIRATION_THRESHOLD; // in seconds, 1 minute before expiration
			if (ts > expirationTime) expirationTime = ts;
			htobe32buf (m_Buffer + offset, ts);
			offset += 4; // end date
		}	
		// update expiration
		SetExpirationTime (expirationTime*1000LL);	
		auto expires = expirationTime - timestamp;
		htobe16buf (expiresBuf, expires > 0 ? expires : 0);	
		// sign
		keys.Sign (m_Buffer, offset, m_Buffer + offset); // LS + leading store type
	}

	LocalLeaseSet2::LocalLeaseSet2 (uint8_t storeType, std::shared_ptr<const IdentityEx> identity, const uint8_t * buf, size_t len):
		LocalLeaseSet (identity, nullptr, 0)
	{
		m_BufferLen = len;
		m_Buffer = new uint8_t[m_BufferLen + 1];
		memcpy (m_Buffer + 1, buf, len);
		m_Buffer[0] = storeType;
	}

	LocalEncryptedLeaseSet2::LocalEncryptedLeaseSet2 (std::shared_ptr<const LocalLeaseSet2> ls, const dotnet::data::PrivateKeys& keys, dotnet::data::SigningKeyType blindedKeyType):
		LocalLeaseSet2 (ls->GetIdentity ()), m_InnerLeaseSet (ls)
	{
		size_t lenInnerPlaintext = ls->GetBufferLen () + 1, lenOuterPlaintext = lenInnerPlaintext + 32 + 1,
			lenOuterCiphertext = lenOuterPlaintext + 32;
		m_BufferLen = 2/*blinded sig type*/ + 32/*blinded pub key*/ + 4/*published*/ + 2/*expires*/ + 2/*flags*/ + 2/*lenOuterCiphertext*/ + lenOuterCiphertext + 64/*signature*/;
		m_Buffer = new uint8_t[m_BufferLen + 1]; 
		m_Buffer[0] = NETDB_STORE_TYPE_ENCRYPTED_LEASESET2;
		BlindedPublicKey blindedKey (ls->GetIdentity ());
		auto timestamp = dotnet::util::GetSecondsSinceEpoch ();	
		char date[9];
		dotnet::util::GetDateString (timestamp, date);
		uint8_t blindedPriv[32], blindedPub[32];
		blindedKey.BlindPrivateKey (keys.GetSigningPrivateKey (), date, blindedPriv, blindedPub);
		std::unique_ptr<dotnet::crypto::Signer> blindedSigner (dotnet::data::PrivateKeys::CreateSigner (blindedKeyType, blindedPriv));
		auto offset = 1;
		htobe16buf (m_Buffer + offset, blindedKeyType); offset += 2; // Blinded Public Key Sig Type
		memcpy (m_Buffer + offset, blindedPub, 32); offset += 32; // Blinded Public Key
		htobe32buf (m_Buffer + offset, timestamp); offset += 4; // published timestamp (seconds)
		auto nextMidnight = (timestamp/86400LL + 1)*86400LL; // 86400 = 24*3600 seconds
		auto expirationTime = ls->GetExpirationTime ()/1000LL; 
		if (expirationTime > nextMidnight) expirationTime = nextMidnight;
		SetExpirationTime (expirationTime*1000LL);
		htobe16buf (m_Buffer + offset, expirationTime > timestamp ? expirationTime - timestamp : 0); offset += 2; // expires
		uint16_t flags = 0;
		htobe16buf (m_Buffer + offset, flags); offset += 2; // flags	
		htobe16buf (m_Buffer + offset, lenOuterCiphertext); offset += 2; // lenOuterCiphertext
		// outerChipherText
		// Layer 1	
		uint8_t subcredential[36];
		blindedKey.GetSubcredential (blindedPub, 32, subcredential);
		htobe32buf (subcredential + 32, timestamp); // outerInput = subcredential || publishedTimestamp
		// keys = HKDF(outerSalt, outerInput, "ELS2_L1K", 44)
		uint8_t keys1[64]; // 44 bytes actual data
		RAND_bytes (m_Buffer + offset, 32); // outerSalt = CSRNG(32)	
		dotnet::crypto::HKDF (m_Buffer + offset, subcredential, 36, "ELS2_L1K", keys1);
		offset += 32; // outerSalt
		uint8_t * outerPlainText = m_Buffer + offset;	
		m_Buffer[offset] = 0; offset++; // flag
		// Layer 2
		// keys = HKDF(outerSalt, outerInput, "ELS2_L2K", 44)
		uint8_t keys2[64]; // 44 bytes actual data
		RAND_bytes (m_Buffer + offset, 32); // innerSalt = CSRNG(32)	
		dotnet::crypto::HKDF (m_Buffer + offset, subcredential, 36, "ELS2_L2K", keys2);
		offset += 32; // innerSalt 
		m_Buffer[offset] = ls->GetStoreType (); 
		memcpy (m_Buffer + offset + 1, ls->GetBuffer (), ls->GetBufferLen ());
		dotnet::crypto::ChaCha20 (m_Buffer + offset, lenInnerPlaintext, keys2, keys2 + 32, m_Buffer + offset); // encrypt Layer 2
		offset += lenInnerPlaintext;
		dotnet::crypto::ChaCha20 (outerPlainText, lenOuterPlaintext, keys1, keys1 + 32, outerPlainText); // encrypt Layer 1
		// signature
		blindedSigner->Sign (m_Buffer, offset, m_Buffer + offset);
		// store hash
		m_StoreHash = blindedKey.GetStoreHash (date);		
	}

	LocalEncryptedLeaseSet2::LocalEncryptedLeaseSet2 (std::shared_ptr<const IdentityEx> identity, const uint8_t * buf, size_t len):
		LocalLeaseSet2 (NETDB_STORE_TYPE_ENCRYPTED_LEASESET2, identity, buf, len) 
	{
		// fill inner LeaseSet2 
		auto blindedKey = std::make_shared<BlindedPublicKey>(identity);	
		dotnet::data::LeaseSet2 ls (buf, len, blindedKey); // inner layer
		if (ls.IsValid ())
		{
			m_InnerLeaseSet = std::make_shared<LocalLeaseSet2>(ls.GetStoreType (), identity, ls.GetBuffer (), ls.GetBufferLen ());
			m_StoreHash = blindedKey->GetStoreHash ();
		}
		else
			LogPrint (eLogError, "LeaseSet2: couldn't extract inner layer");			
	}
	
}
}
