/*
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org/>
*/

#include "util.h"

#include "../lib/SimpleTinyScript/sts_embedding_extras.h"

#include <onion/log.h>
#include <onion/onion.h>
#include <onion/shortcuts.h>
#include <onion/response.h>
#include <onion/handler.h>
#include <onion/dict.h>
#include <onion/block.h>

#include <hiredis/hiredis.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


sts_value_t *stsml_value_from_redis(sts_script_t *script, redisReply *reply)
{
	sts_value_t *ret = NULL, *temp = NULL;
	size_t i;


	switch(reply->type)
	{
		case REDIS_REPLY_INTEGER:
			if(!(ret = sts_value_from_number(script, (double)reply->integer)))
			{
				fprintf(stderr, "could not create new ret number\n");
				return NULL;
			}
		break;
		case REDIS_REPLY_ERROR:
			/* TODO set some local with the error message */
		case REDIS_REPLY_NIL:
			if(!(ret = sts_value_create(script, STS_NIL)))
			{
				fprintf(stderr, "could not create new ret nil value\n");
				return NULL;
			}
		break;
		case REDIS_REPLY_STATUS:
		case REDIS_REPLY_STRING:
			if(!(ret = sts_value_from_nstring(script, reply->str, reply->len)))
			{
				fprintf(stderr, "could not create new ret string\n");
				return NULL;
			}
		break;
		case REDIS_REPLY_ARRAY:
			if(!(ret = sts_value_create(script, STS_ARRAY)))
			{
				fprintf(stderr, "could not create new ret array\n");
				return NULL;
			}

			for(i = 0; i < reply->elements; ++i)
			{
				if(!(temp = stsml_value_from_redis(script, reply->element[i])))
				{
					fprintf(stderr, "could not create a value from nested reply");

					if(ret)
						sts_value_reference_decrement(script, ret);

					return NULL;
				}

				sts_array_append_insert(script, ret, temp, ret->array.length);
			}
		break;
	}

	return ret;
}