/*
* Copyright (c) 2013-2020, The PurpleI2P Project
*
* This file is part of Purple i2pd project and licensed under BSD3
*
* See full license text in LICENSE file at top of project tree
*/

#include "api.h"
#include "capi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>


// Uses the example from: https://stackoverflow.com/a/9210560
// See also https://stackoverflow.com/questions/9210528/split-string-with-delimiters-in-c/9210560#
// Does not handle consecutive delimiters, this is only for passing
// lists of arguments by value to InitI2P from C_InitI2P
char** str_split(char* a_str, const char a_delim)
{
    char** result    = 0;
    size_t count     = 0;
    char* tmp        = a_str;
    char* last_comma = 0;
    char delim[2];
    delim[0] = a_delim;
    delim[1] = 0;

    /* Count how many elements will be extracted. */
    while (*tmp)
    {
        if (a_delim == *tmp)
        {
            count++;
            last_comma = tmp;
        }
        tmp++;
    }

    /* Add space for trailing token. */
    count += last_comma < (a_str + strlen(a_str) - 1);

    /* Add space for terminating null string so caller
       knows where the list of returned strings ends. */
    count++;

    result = (char**) malloc(sizeof(char*) * count);

    if (result)
    {
        size_t idx  = 0;
        char* token = strtok(a_str, delim);

        while (token)
        {
            assert(idx < count);
            *(result + idx++) = strdup(token);
            token = strtok(0, delim);
        }
        assert(idx == count - 1);
        *(result + idx) = 0;
    }

    return result;
}


#ifdef __cplusplus
extern "C" {
#endif

void C_InitI2P (int argc, char argv[], const char * appName)
{
	const char* delim = " ";
	char* vargs = strdup(argv);
	char** args = str_split(vargs, *delim);
	std::cout << argv;
	return i2p::api::InitI2P(argc, args, appName);
}

void C_TerminateI2P ()
{
	return i2p::api::TerminateI2P();
}

void C_StartI2P ()
{
	std::shared_ptr<std::ostream> logStream;
	return i2p::api::StartI2P(logStream);
}

void C_StopI2P ()
{
	return i2p::api::StopI2P();
}

void C_RunPeerTest ()
{
	return i2p::api::RunPeerTest();
}

#ifdef __cplusplus
}
#endif
