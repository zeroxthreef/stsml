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
#include "parser.h"

#include "../lib/SimpleTinyScript/sts_embedding_extras.h"

#include <onion/log.h>
#include <onion/onion.h>
#include <onion/shortcuts.h>
#include <onion/response.h>
#include <onion/handler.h>
#include <onion/dict.h>
#include <onion/block.h>

#include <hiredis/hiredis.h>

#ifdef COMPILING
	#include "../stdlib.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>



/* task argument struct */
typedef struct
{
	char *script_path;
	sts_value_t *args;
} stsml_task_args_t;

/* arg struct */
typedef struct
{
	int present;
	char *name, *description, *value;
} stsml_args_t;

/* contain local info for scripts */
typedef struct
{
	sts_scope_t *locals;
} stsml_script_t;


typedef struct
{
	sts_script_t *script;
	stsml_parser_ctx_t *parser;

	sts_map_row_t *script_locals;

	onion_request *req;
	onion_response *res;

	redisContext *redis_ctx;

	sts_value_t *cleanup, *response_str, *response_file, *respond_redirect;

	int http_status;

	char *last_resort;
} stsml_ctx_t;


char *read_file(sts_script_t *script, char *file, unsigned int *size);
char *import(sts_script_t *script, char *file);
sts_value_t *cli_actions(sts_script_t *script, sts_value_t *action, sts_node_t *args, sts_scope_t *locals, sts_value_t **previous);



sts_value_t *server_actions(sts_script_t *script, sts_value_t *action, sts_node_t *args, sts_scope_t *locals, sts_value_t **previous);
char *stsml_import(sts_script_t *script, char *file);

onion_connection_status respond_index(void *data, onion_request *req, onion_response *res)
{
	struct stat st;
	char *final_path = NULL;

	#define CHECK_INDEX(extension) do{	\
		stsml_asprintf(&final_path, "%s/index." extension, (strlen(onion_request_get_fullpath(req)) <= 1) ? "./" : ( &onion_request_get_fullpath(req)[(onion_request_get_fullpath(req)[0] == '/') ? 1 : 0] ));	\
		if(!stat(final_path, &st) && S_ISREG(st.st_mode))	\
		{	\
			ONION_INFO("redirecting to %s", final_path);	\
			onion_shortcut_internal_redirect(final_path, req, res);	\
			free(final_path);	\
			return OCS_PROCESSED;	\
		}	\
	}while(0)


	/* also check for '../' '~/' */
	if(strstr(onion_request_get_fullpath(req), "../") || strstr(onion_request_get_fullpath(req), "~/"))
	{
		onion_response_set_code(res, 400);
		onion_response_printf(res, "improperly formatted url");
		
		return OCS_PROCESSED;
	}


	/* test if it's a directory */
	if(!stat(  (strlen(onion_request_get_fullpath(req)) <= 1) ? "." : ( &onion_request_get_fullpath(req)[(onion_request_get_fullpath(req)[0] == '/') ? 1 : 0] ) , &st) && S_ISDIR(st.st_mode))
	{
		ONION_DEBUG("client requested a directory, checking for an index");

		CHECK_INDEX("stsml");
		CHECK_INDEX("html");
		CHECK_INDEX("xhtml");
		CHECK_INDEX("htm");
		CHECK_INDEX("txt");

		if(final_path) free(final_path);
	}

	return OCS_NOT_PROCESSED;
}

onion_connection_status respond_file_specific(void *data, onion_request *req, onion_response *res)
{
	struct stat st;
	/* determine how to continue handling files depending on the type */

	if(!(strrchr(onion_request_get_fullpath(req), '.') && !strcmp(strrchr(onion_request_get_fullpath(req), '.'), ".stsml")))
	{
		if(!stat(  (strlen(onion_request_get_fullpath(req)) <= 1) ? "." : ( &onion_request_get_fullpath(req)[(onion_request_get_fullpath(req)[0] == '/') ? 1 : 0] ) , &st) && S_ISREG(st.st_mode))
		{
			ONION_DEBUG("responding as a file '%s'", onion_request_get_fullpath(req));
			onion_shortcut_response_file((strlen(onion_request_get_fullpath(req)) <= 1) ? "." : ( &onion_request_get_fullpath(req)[(onion_request_get_fullpath(req)[0] == '/') ? 1 : 0] ), req, res);

			return OCS_PROCESSED;
		}
	}

	return OCS_NOT_PROCESSED;
}

onion_connection_status respond_stsml(void *data, onion_request *req, onion_response *res)
{
	size_t size;
	char *script_file = NULL, *parsed_stsml = NULL, *script_path = NULL, *temp_str = NULL, *redirect = NULL;
	unsigned int line = 0, offset = 0;
	stsml_ctx_t *stsml_ctx = (stsml_ctx_t *)data;
	sts_value_t *ret_val = NULL;
	sts_map_row_t *row = NULL;
	stsml_script_t *local_ctx = NULL;


	script_path = (char *)&(onion_request_get_fullpath(req)[(onion_request_get_fullpath(req)[0] == '/') ? 1 : 0]);

	/* only respond if a .stsml file */
	if(strlen(onion_request_get_fullpath(req)) > 1 && strrchr(script_path, '.') && !strcmp(strrchr(script_path, '.'), ".stsml"))
	{
		ONION_INFO("executing script %s", script_path);


		/* setup stsml ctx */

		stsml_ctx->req = req;
		stsml_ctx->res = res;
		stsml_ctx->http_status = 200;


		if(stsml_ctx->script->script)
			sts_ast_delete(stsml_ctx->script, stsml_ctx->script->script);


		if(!(stsml_ctx->response_str = sts_value_create(stsml_ctx->script, STS_STRING)))
		{
			ONION_ERROR("could not initialize stsml ctx");
			return OCS_NOT_PROCESSED;
		}

		if(!(stsml_ctx->response_file = sts_value_create(stsml_ctx->script, STS_STRING)))
		{
			ONION_ERROR("could not initialize stsml ctx");
			return OCS_NOT_PROCESSED;
		}

		if(!(stsml_ctx->respond_redirect = sts_value_create(stsml_ctx->script, STS_STRING)))
		{
			ONION_ERROR("could not initialize stsml ctx");
			return OCS_NOT_PROCESSED;
		}

		if(!(stsml_ctx->cleanup = sts_value_create(stsml_ctx->script, STS_ARRAY)))
		{
			ONION_ERROR("could not initialize stsml ctx");
			return OCS_NOT_PROCESSED;
		}

		/* read script file */

		if(!(script_file = read_file(stsml_ctx->script, script_path, (unsigned int *)&size)))
		{
			ONION_ERROR("could not read script file at %s", script_path);
			return OCS_NOT_PROCESSED;
		}

		/* parse stsml into sts */

		stsml_parser_init(stsml_ctx->parser);

		if(!(temp_str = stsml_parser_pwd_from_file(script_path)))
		{
			ONION_ERROR("could not parse stsml into sts script %s", script_path);
			free(script_file);

			/* TODO make custom error responses */
			onion_response_set_code(res, 500);
			onion_response_printf(res, "could not parse stsml '%s' into sts script", script_path);
		}

		if(stsml_parser_run(stsml_ctx->parser, script_file, temp_str))
		{
			ONION_ERROR("could not parse stsml into sts script %s", script_path);
			free(script_file);
			free(temp_str);

			/* TODO make custom error responses */
			onion_response_set_code(res, 500);
			onion_response_printf(res, "could not parse stsml '%s' into sts script", script_path);

			return OCS_PROCESSED;
		}

		free(temp_str);


		parsed_stsml = stsml_ctx->parser->assembled;

		/* printf("DEBUG PARSED VIEW: '%s' '%s'\n", script_file, parsed_stsml); */

		/* look for local struct */

		if(!stsml_ctx->script_locals || !(row = sts_map_get(&stsml_ctx->script_locals, script_path, strlen(script_path))))
		{
			/* if it does not exist, make a new one */
			ONION_INFO("creating new locals struct for script %s", script_path);

			if(!(local_ctx = calloc(1, sizeof(stsml_script_t))))
			{
				ONION_ERROR("could not create locals for %s", script_path);
				free(script_file);
				free(parsed_stsml);

				/* TODO make custom error responses */
				onion_response_set_code(res, 500);
				onion_response_printf(res, "could not parse script '%s', line: %u, character offset: %u", script_path, line, offset);

				return OCS_PROCESSED;
			}

			if(!(row = sts_map_add_set(&stsml_ctx->script_locals, script_path, strlen(script_path), local_ctx)))
			{
				ONION_ERROR("could not add new locals to stsml locals for %s", script_path);
				free(script_file);
				free(parsed_stsml);
				free(local_ctx);

				/* TODO make custom error responses */
				onion_response_set_code(res, 500);
				onion_response_printf(res, "could not parse script '%s', line: %u, character offset: %u", script_path, line, offset);

				return OCS_PROCESSED;
			}

			row->type = STS_ROW_VOID;

			/* need to make globals the uplevel */
			if(!(local_ctx->locals = sts_scope_push(stsml_ctx->script, stsml_ctx->script->globals)))
			{
				ONION_ERROR("could not push new locals to stsml locals for %s", script_path);
				free(script_file);
				free(parsed_stsml);
				free(local_ctx);

				
				onion_response_set_code(res, 500);
				onion_response_printf(res, "could not parse script '%s', line: %u, character offset: %u", script_path, line, offset);

				return OCS_PROCESSED;
			}
		}
		else
		{
			ONION_INFO("found locals struct for script %s", script_path);

			local_ctx = row->value;
		}

		/* run through sts */

		if(!(stsml_ctx->script->script = sts_parse(stsml_ctx->script, NULL, parsed_stsml, script_path, &offset, &line)))
		{
			ONION_ERROR("could not parse script %s", script_path);
			free(script_file);
			free(parsed_stsml);

			
			onion_response_set_code(res, 500);
			onion_response_printf(res, "could not parse script '%s', line: %u, character offset: %u", script_path, line, offset);

			return OCS_PROCESSED;
		}

		free(script_file);
		free(parsed_stsml);

		if(!(ret_val = sts_eval(stsml_ctx->script, stsml_ctx->script->script, local_ctx->locals, NULL, 0, 0)))
		{
			ONION_ERROR("could not eval script %s", script_path);

			sts_ast_delete(stsml_ctx->script, stsml_ctx->script->script);

			
			onion_response_set_code(res, 500);
			onion_response_printf(res, "could not eval script '%s'", script_path);

			return OCS_PROCESSED;
		}
		
		if(!sts_value_reference_decrement(stsml_ctx->script, ret_val))
			ONION_ERROR("could not refdec return value for script %s", script_path);
		

		/* respond */

		onion_response_set_code(res, stsml_ctx->http_status);


		if(stsml_ctx->respond_redirect->string.length)
			redirect = sts_memdup(stsml_ctx->respond_redirect->string.data, stsml_ctx->respond_redirect->string.length);
		else if(stsml_ctx->response_file->string.length)
			onion_shortcut_response_file(stsml_ctx->response_file->string.data, req, res);
		else if(stsml_ctx->response_str->string.length)
			onion_response_write(res, stsml_ctx->response_str->string.data, stsml_ctx->response_str->string.length);
		else
			onion_response_write(res, "", 0);


		/* cleanup */

		if(!sts_value_reference_decrement(stsml_ctx->script, stsml_ctx->response_file))
			ONION_ERROR("could not refdec file response string value for script %s", script_path);
		
		if(!sts_value_reference_decrement(stsml_ctx->script, stsml_ctx->response_str))
			ONION_ERROR("could not refdec response string value for script %s", script_path);

		if(!sts_value_reference_decrement(stsml_ctx->script, stsml_ctx->respond_redirect))
			ONION_ERROR("could not refdec redirect string value for script %s", script_path);

		if(!sts_value_reference_decrement(stsml_ctx->script, stsml_ctx->cleanup))
			ONION_ERROR("could not refdec cleanup array value for script %s", script_path);


		/* unfortunately have to do this because the stsml ctx will be reused. Obviously theres a much better way to do this, but I just want something that works well enough */
		if(redirect)
		{
			onion_shortcut_internal_redirect(redirect, req, res);
			free(redirect);
		}

		return OCS_PROCESSED;
	}

	return OCS_NOT_PROCESSED;
}

onion_connection_status respond_last_resort(void *data, onion_request *req, onion_response *res)
{
	/* controlling how a 404 and alike are displayed is done here */
	stsml_ctx_t *stsml_ctx = (stsml_ctx_t *)data;


	if(stsml_ctx->last_resort)
		return onion_shortcut_internal_redirect(stsml_ctx->last_resort, req, res);

	return OCS_NOT_PROCESSED;
}

void *start_task(stsml_task_args_t *args_pass)
{
	char *script_path = args_pass->script_path;
	sts_value_t *args = args_pass->args;
	unsigned long script_text_size = 0, offset = 0, line = 0;
	char *script_text = NULL;
	stsml_parser_ctx_t parser;
	stsml_ctx_t ctx;
	sts_value_t *res = NULL;
	sts_script_t script;


	ONION_INFO("starting new task from script '%s'", script_path);

	/* initialize the script */

	memset(&script, 0, sizeof(sts_script_t));

	/* set a read file callback */
	script.read_file = &read_file;

	/* initialize the router */
	script.router = &server_actions;

	script.import_file = &stsml_import;

	script.userdata = &ctx;

	/* initialize the stsml_ctx */

	memset(&ctx, 0, sizeof(stsml_ctx_t));

	ctx.parser = &parser;
	ctx.script = &script;

	if(!(script_text = read_file(&script, script_path, &script_text_size)))
	{
		ONION_ERROR("could not read script '%s' in new task", script_path);
		goto cleanup;
	}


	if(!(script.script = sts_parse(&script, NULL, script_text, script_path, &offset, &line)))
	{
		ONION_ERROR("could not parse script '%s' in new task", script_path);
		goto cleanup;
	}

	/* add the arguments to the explicitly created global scope */

	if(!(script.globals = sts_scope_push(&script, NULL)))
	{
		ONION_ERROR("could not create global scope level in new task");
		goto cleanup;
	}


	if(!sts_map_add_set(&script.globals->locals, "args", strlen("args"), args))
	{
		ONION_ERROR("could not create args in global scope level in new task");
		goto cleanup;
	}

	/* evaluate script */

	if(!(res = sts_eval(&script, script.script, NULL, NULL, 0, 0)))
	{
		ONION_ERROR("could not eval script in new task");
		goto cleanup;
	}


	/* cleanup */
cleanup:

	if(ctx.redis_ctx)
		redisFree(ctx.redis_ctx);

	if(res)
		sts_value_reference_decrement(&script, res);

	sts_destroy(&script);

	if(script_text)
		free(script_text);

	free(args_pass->script_path);
	/* dont need to free the args array because the global scope owns it */

	free(args_pass);

	pthread_detach(pthread_self());

	return NULL;
}

sts_value_t *server_actions(sts_script_t *script, sts_value_t *action, sts_node_t *args, sts_scope_t *locals, sts_value_t **previous)
{
	sts_value_t *ret = NULL, *eval_value = NULL, *temp_value = NULL, *first_arg_value = NULL, *second_arg_value = NULL;
	FILE *proc_pipe = NULL, *file = NULL;
	char *temp_str = NULL;
	unsigned int i = 0, size = 0, total = 0, temp_uint = 0;
	stsml_ctx_t *stsml_ctx = NULL;
	onion_block *data;
	stsml_task_args_t *task_args = NULL;
	pthread_t id;
	redisReply *reply = NULL;
	size_t redis_args_size[1024], redis_args_cleanup[1024];
	char *redis_args[1024];



	#define STS_ARRAY_RESIZE(value_ptr, size) do{	\
			if(!((value_ptr)->array.data = realloc((value_ptr)->array.data, (size) * sizeof(sts_value_t **)))) {fprintf(stderr, "could not resize array");}	\
			else (value_ptr)->array.allocated = (size);	\
		}while(0)

	#define STS_ARRAY_APPEND_INSERT(value_ptr, value_insert, position) do{	\
			if((value_ptr)->array.length + 1 > (value_ptr)->array.allocated) STS_ARRAY_RESIZE((value_ptr), (value_ptr)->array.length + 1);	\
			if(position >= (value_ptr)->array.length) (value_ptr)->array.data[(value_ptr)->array.length] = (value_insert);	\
			else{ memmove(&(value_ptr)->array.data[position + 1], &(value_ptr)->array.data[position], ((value_ptr)->array.length - position) * sizeof(sts_value_t **)); (value_ptr)->array.data[position] = (value_insert);}	\
			(value_ptr)->array.length++;	\
		}while(0)

	#define STS_STRING_ASSEMBLE(dest, current_size, middle_str, middle_size, end_str, end_size) do{	\
			if(!((dest) = realloc((dest), (current_size) + (middle_size) + (end_size) + 1))) fprintf(stderr, "could not resize assembled string");	\
			memmove(&(dest)[current_size], (middle_str), (middle_size));	\
			memmove(&(dest)[current_size + (middle_size)], (end_str), (end_size));	\
			(dest)[current_size + (middle_size) + (end_size)] = 0x0;	\
			current_size += (middle_size) + (end_size);	\
		}while(0)
	
	#ifdef STS_GOTO_JIT
		#define GOTO_LABEL_CAT_(a, b) a ## b
		#define GOTO_LABEL_CAT(a, b) GOTO_LABEL_CAT_(a, b)
		#define GOTO_SET(id) do{args->label = && GOTO_LABEL_CAT(sts_jit_, __LINE__); args->router_id = (void *)(id); GOTO_LABEL_CAT(sts_jit_, __LINE__):;}while(0)
		#define GOTO_JMP(id) do{if(args->router_id == (void *)(id)){goto *(args->label);}}while(0)
		#define GOTO_ACTIVATED (args->router_id)
	#else
		#define GOTO_SET(id)
		#define GOTO_JMP(id)
		#define GOTO_ACTIVATED
	#endif



	stsml_ctx = script->userdata;

	/* printf("=========== %p %p %s\n", script->globals, locals, action->string.data); */
	GOTO_JMP(&server_actions);
	if(!GOTO_ACTIVATED && action->type == STS_STRING)
	{
		if(!strcmp("http-write", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next)
			{
				if(!(eval_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-write\n");
					return NULL;
				}
				else if(eval_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in http-write is not a string\n");
					return NULL;
				}
				else
				{
					STS_STRING_ASSEMBLE(stsml_ctx->response_str->string.data, stsml_ctx->response_str->string.length, eval_value->string.data, eval_value->string.length, "", 0);

					if(!(ret = sts_value_from_number(script, 1.0)))
					{
						fprintf(stderr, "could not create new ret number\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}

				/* === */
				if(eval_value)
					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
			}
			else
			{
				fprintf(stderr, "http-write requires 1 string argument\n");
				return NULL;
			}
		}
		else if(!strcmp("http-clear", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(stsml_ctx->response_str->string.length)
			{
				stsml_ctx->response_str->string.data[0] = 0x0;
				stsml_ctx->response_str->string.length = 0;
			}
			
			if(!(ret = sts_value_from_number(script, 1.0)))
			{
				fprintf(stderr, "could not create new ret number\n");
				return NULL;
			}
		}
		else if(!strcmp("http-method-get", action->string.data))
		{
			GOTO_SET(&server_actions);
			#define METHOD_CASE(m) case OR_##m : if(!(ret = sts_value_from_string(script, #m)))	\
			{	\
				fprintf(stderr, "could not create new ret string\n");	\
				return NULL;	\
			} break

			switch(onion_request_get_flags(stsml_ctx->req) & OR_METHODS)
			{
				METHOD_CASE(GET);
				METHOD_CASE(POST);
				METHOD_CASE(HEAD);
				METHOD_CASE(OPTIONS);
				METHOD_CASE(PROPFIND);
				METHOD_CASE(PUT);
				METHOD_CASE(DELETE);
				METHOD_CASE(MOVE);
				METHOD_CASE(MKCOL);
				METHOD_CASE(PROPPATCH);
				METHOD_CASE(PATCH);
				default:
					ONION_ERROR("invalid http method");
					return NULL;
			}
		}
		else if(!strcmp("http-path-get", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(!(ret = sts_value_from_string(script, onion_request_get_path(stsml_ctx->req))))
			{
				fprintf(stderr, "could not create new ret string\n");
				return NULL;
			}
		}
		else if(!strcmp("http-body-get", action->string.data))
		{
			GOTO_SET(&server_actions);
			if((data = onion_request_get_data(stsml_ctx->req)))
			{
				if(!(ret = sts_value_from_nstring(script, onion_block_data(data), onion_block_size(data))))
				{
					fprintf(stderr, "could not create new ret string\n");
					return NULL;
				}
			}
			else
			{
				if(!(ret = sts_value_create(script, STS_NIL)))
				{
					fprintf(stderr, "could not create new ret nil\n");
					return NULL;
				}
			}
		}
		else if(!strcmp("http-post-get", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next)
			{
				if(!(eval_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-post-get\n");
					return NULL;
				}
				else if(eval_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in http-post-get is not a string\n");
					return NULL;
				}
				else if(onion_request_get_post(stsml_ctx->req, eval_value->string.data))
				{
					if(!(ret = sts_value_from_string(script, onion_request_get_post(stsml_ctx->req, eval_value->string.data))))
					{
						fprintf(stderr, "could not create new ret string\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}
				else
				{
					if(!(ret = sts_value_create(script, STS_NIL)))
					{
						fprintf(stderr, "could not create new ret nil\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}

				/* === */
				if(eval_value)
					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
			}
			else
			{
				fprintf(stderr, "http-post-get requires 1 string argument\n");
				return NULL;
			}
		}
		else if(!strcmp("http-query-get", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next)
			{
				if(!(eval_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-query-get\n");
					return NULL;
				}
				else if(eval_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in http-query-get is not a string\n");
					return NULL;
				}
				else if(onion_request_get_query(stsml_ctx->req, eval_value->string.data))
				{
					if(!(ret = sts_value_from_string(script, onion_request_get_query(stsml_ctx->req, eval_value->string.data))))
					{
						fprintf(stderr, "could not create new ret string\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}
				else
				{
					if(!(ret = sts_value_create(script, STS_NIL)))
					{
						fprintf(stderr, "could not create new ret nil\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}

				/* === */
				if(eval_value)
					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
			}
			else
			{
				fprintf(stderr, "http-query-get requires 1 string argument\n");
				return NULL;
			}
		}
		else if(!strcmp("http-file-get", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next)
			{
				if(!(eval_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-file-get\n");
					return NULL;
				}
				else if(eval_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in http-file-get is not a string\n");
					return NULL;
				}
				else if(onion_request_get_file(stsml_ctx->req, eval_value->string.data))
				{
					if(!(ret = sts_value_from_string(script, onion_request_get_file(stsml_ctx->req, eval_value->string.data))))
					{
						fprintf(stderr, "could not create new ret string\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}
				else
				{
					if(!(ret = sts_value_create(script, STS_NIL)))
					{
						fprintf(stderr, "could not create new ret nil\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}

				/* === */
				if(eval_value)
					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
			}
			else
			{
				fprintf(stderr, "http-file-get requires 1 string argument\n");
				return NULL;
			}
		}
		else if(!strcmp("http-cookie-get", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next)
			{
				if(!(eval_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-cookie-get\n");
					return NULL;
				}
				else if(eval_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in http-cookie-get is not a string\n");
					return NULL;
				}
				else if(onion_request_get_cookie(stsml_ctx->req, eval_value->string.data))
				{
					if(!(ret = sts_value_from_string(script, onion_request_get_cookie(stsml_ctx->req, eval_value->string.data))))
					{
						fprintf(stderr, "could not create new ret string\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}
				else
				{
					if(!(ret = sts_value_create(script, STS_NIL)))
					{
						fprintf(stderr, "could not create new ret nil\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}

				/* === */
				if(eval_value)
					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
			}
			else
			{
				fprintf(stderr, "http-cookie-get requires 1 string argument\n");
				return NULL;
			}
		}
		/* http-cookie-put string name, string value, number time_valid(added to current time, if 0 expire now, if -1 never expire), number flags */
		else if(!strcmp("http-cookie-put", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next && args->next->next && args->next->next->next && args->next->next->next->next)
			{
				if(!(first_arg_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-cookie-put\n");
					return NULL;
				}
				else if(first_arg_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in http-cookie-put is not a string\n");
					return NULL;
				}

				if(!(second_arg_value = sts_eval(script, args->next->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-cookie-put\n");
					return NULL;
				}
				else if(second_arg_value->type != STS_STRING)
				{
					fprintf(stderr, "second argument in http-cookie-put is not a string\n");
					return NULL;
				}

				if(!(temp_value = sts_eval(script, args->next->next->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-cookie-put\n");
					return NULL;
				}
				else if(temp_value->type != STS_NUMBER)
				{
					fprintf(stderr, "third argument in http-cookie-put is not a number\n");
					return NULL;
				}

				if(!(eval_value = sts_eval(script, args->next->next->next->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-cookie-put\n");
					return NULL;
				}
				else if(eval_value->type != STS_NUMBER)
				{
					fprintf(stderr, "fourth argument in http-cookie-put is not a number\n");
					return NULL;
				}

				if(!(ret = sts_value_from_number(script, (double)onion_response_add_cookie(stsml_ctx->res, first_arg_value->string.data, second_arg_value->string.data, (long)temp_value->number, NULL, NULL, (int)eval_value->number) )))
				{
					fprintf(stderr, "could not create new ret number\n");

					if(!sts_value_reference_decrement(script, first_arg_value))
						fprintf(stderr, "could not refdec the argument\n");
					
					if(!sts_value_reference_decrement(script, second_arg_value))
						fprintf(stderr, "could not refdec the argument\n");
					
					if(!sts_value_reference_decrement(script, temp_value))
						fprintf(stderr, "could not refdec the argument\n");

					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
						
					return NULL;
				}

				/* add to cleanup array because the reference needs to be +1 until the http body is sent */
				STS_ARRAY_APPEND_INSERT(stsml_ctx->cleanup, first_arg_value, 0);
				STS_ARRAY_APPEND_INSERT(stsml_ctx->cleanup, second_arg_value, 0);

				first_arg_value->references++;
				second_arg_value->references++;


				if(!sts_value_reference_decrement(script, first_arg_value))
						fprintf(stderr, "could not refdec the argument\n");
					
				if(!sts_value_reference_decrement(script, second_arg_value))
					fprintf(stderr, "could not refdec the argument\n");
				
				if(!sts_value_reference_decrement(script, temp_value))
					fprintf(stderr, "could not refdec the argument\n");

				if(!sts_value_reference_decrement(script, eval_value))
					fprintf(stderr, "could not refdec the argument\n");
			}
			else
			{
				fprintf(stderr, "http-cookie-put requires 2 string arguments and 2 number arguments\n");
				return NULL;
			}
		}
		else if(!strcmp("http-header-get", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next)
			{
				if(!(eval_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-header-get\n");
					return NULL;
				}
				else if(eval_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in http-header-get is not a string\n");
					return NULL;
				}
				else if(onion_request_get_header(stsml_ctx->req, eval_value->string.data))
				{
					if(!(ret = sts_value_from_string(script, onion_request_get_header(stsml_ctx->req, eval_value->string.data))))
					{
						fprintf(stderr, "could not create new ret string\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}
				else
				{
					if(!(ret = sts_value_create(script, STS_NIL)))
					{
						fprintf(stderr, "could not create new ret nil\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}

				/* === */
				if(eval_value)
					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
			}
			else
			{
				fprintf(stderr, "http-header-get requires 1 string argument\n");
				return NULL;
			}
		}
		else if(!strcmp("http-header-put", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next && args->next->next)
			{
				if(!(first_arg_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-header-put\n");
					return NULL;
				}
				else if(first_arg_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in http-header-put is not a string\n");
					return NULL;
				}

				if(!(eval_value = sts_eval(script, args->next->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-header-put\n");
					return NULL;
				}
				else if(eval_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in http-header-put is not a string\n");
					return NULL;
				}


				onion_response_set_header(stsml_ctx->res, first_arg_value->string.data, eval_value->string.data);


				if(!(ret = sts_value_from_number(script, 1.0)))
				{
					fprintf(stderr, "could not create new ret number\n");

					if(!sts_value_reference_decrement(script, first_arg_value))
						fprintf(stderr, "could not refdec the argument\n");

					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
						
					return NULL;
				}

				/* add to cleanup array because the reference needs to be +1 until the http body is sent */
				STS_ARRAY_APPEND_INSERT(stsml_ctx->cleanup, first_arg_value, 0);
				STS_ARRAY_APPEND_INSERT(stsml_ctx->cleanup, eval_value, 0);

				first_arg_value->references++;
				eval_value->references++;


				if(first_arg_value)
					if(!sts_value_reference_decrement(script, first_arg_value))
						fprintf(stderr, "could not refdec the argument\n");
				
				if(eval_value)
					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
			}
			else
			{
				fprintf(stderr, "http-header-put requires 2 string arguments\n");
				return NULL;
			}
		}
		else if(!strcmp("http-write-file", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next)
			{
				if(!(eval_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-write-file\n");
					return NULL;
				}
				else if(eval_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in http-write-file is not a string\n");
					return NULL;
				}
				else
				{
					if(sts_value_copy(script, stsml_ctx->response_file, eval_value, 0))
					{
						fprintf(stderr, "could not set file path response string in http-write-file\n");
						return NULL;
					}


					if(!(ret = sts_value_from_number(script, 1.0)))
					{
						fprintf(stderr, "could not create new ret number\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}

				/* === */
				if(eval_value)
					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
			}
			else
			{
				fprintf(stderr, "http-write-file requires 1 string argument\n");
				return NULL;
			}
		}
		else if(!strcmp("http-route", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next)
			{
				if(!(eval_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval argument in http-route\n");
					return NULL;
				}
				else if(eval_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in http-route is not a string\n");
					return NULL;
				}
				else
				{
					if(sts_value_copy(script, stsml_ctx->respond_redirect, eval_value, 0))
					{
						fprintf(stderr, "could not set file path response string in http-route\n");
						return NULL;
					}

					if(!(ret = sts_value_from_number(script, 1.0)))
					{
						fprintf(stderr, "could not create new ret number\n");

						if(!sts_value_reference_decrement(script, eval_value))
							fprintf(stderr, "could not refdec the argument\n");
							
						return NULL;
					}
				}

				/* === */
				if(eval_value)
					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
			}
			else
			{
				fprintf(stderr, "http-route requires 1 string argument\n");
				return NULL;
			}
		}
		else if(!strcmp("redis-connect", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next && args->next->next)
			{
				if(!(first_arg_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval first argument in redis-connect\n");
					return NULL;
				}
				else if(first_arg_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in redis-connect is not a string\n");
					return NULL;
				}

				if(!(eval_value = sts_eval(script, args->next->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval second argument in redis-connect\n");
					return NULL;
				}
				else if(eval_value->type != STS_NUMBER)
				{
					fprintf(stderr, "second argument in redis-connect is not a number\n");
					return NULL;
				}


				if(stsml_ctx->redis_ctx)
					redisFree(stsml_ctx->redis_ctx);

				if(!(stsml_ctx->redis_ctx = redisConnect(first_arg_value->string.data, (int)eval_value->number)) || stsml_ctx->redis_ctx->err)
					fprintf(stderr, "could not connect to redis server at '%s'\n", first_arg_value->string.data);
				

				if(!(ret = sts_value_from_number(script, (stsml_ctx->redis_ctx && !stsml_ctx->redis_ctx->err) ? 1.0 : 0.0)))
				{
					fprintf(stderr, "could not create new ret number\n");

					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");
					
					if(!sts_value_reference_decrement(script, first_arg_value))
						fprintf(stderr, "could not refdec the argument\n");
						
					return NULL;
				}

				/* === */
				if(eval_value)
					if(!sts_value_reference_decrement(script, eval_value))
						fprintf(stderr, "could not refdec the argument\n");

				if(eval_value)
					if(!sts_value_reference_decrement(script, first_arg_value))
						fprintf(stderr, "could not refdec the argument\n");
			}
			else
			{
				fprintf(stderr, "redis-connect requires 1 string argument\n");
				return NULL;
			}
		}
		else if(!strcmp("redis", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next)
			{
				/* have to stash all arguments in a temporary array and destroy this at the end */

				if(!(temp_value = sts_value_create(script, STS_ARRAY)))
				{
					fprintf(stderr, "could not create temporary stash of arguments\n");
					return NULL;
				}

				while((args = args->next))
				{
					if(!(eval_value = sts_eval(script, args, locals, previous, 1, 0)))
					{
						fprintf(stderr, "could not eval argument in redis\n");
						sts_value_reference_decrement(script, temp_value);
						return NULL;
					}


					switch(eval_value->type)
					{
						case STS_STRING:
							sts_array_append_insert(script, temp_value, eval_value, 0);

							redis_args[i] = eval_value->string.data;
							redis_args_size[i] = eval_value->string.length;

							redis_args_cleanup[i] = 0;

							if(++i == 1024) break; /* limit to 1024 arguments, this limit still feels too large */
						break;
						case STS_NUMBER:
							redis_args[i] = NULL;

							stsml_asprintf(&redis_args[i], "%lld", (long)eval_value->number);
							redis_args_size[i] = strlen(redis_args[i]);

							redis_args_cleanup[i] = 1;

							sts_value_reference_decrement(script, eval_value);

							if(++i == 1024) break; /* limit to 1024 arguments, this limit still feels too large */
						break;
					}

					/* printf("redis debug === %u '%s' %llu %llu\n", i - 1, redis_args[i - 1], redis_args_size[i - 1], redis_args_cleanup[i - 1]); */

				}

				temp_uint = i;


				if((reply = redisCommandArgv(stsml_ctx->redis_ctx, temp_uint, (const char **)redis_args, redis_args_size)))
				{
					if(!(ret = stsml_value_from_redis(script, reply)))
					{
						fprintf(stderr, "could not create value from redis reply\n");
						return NULL;
					}

					freeReplyObject(reply);	
				}
				else
				{
					if(!(ret = sts_value_create(script, STS_NIL)))
					{
						fprintf(stderr, "reply error in redis action\n");
						return NULL;
					}
				}

				/* cleanup */
				for(i = 0; i < temp_uint; ++i)
				{
					if(redis_args_cleanup[i])
						free(redis_args[i]);
				}

				if(!sts_value_reference_decrement(script, temp_value))
				{
					fprintf(stderr, "could not clean up redis argument stash\n");
					return NULL;
				}

				/* === */
			}
			else
			{
				fprintf(stderr, "redis requires at least 1 argument\n");
				return NULL;
			}
		}
		else if(!strcmp("task-create", action->string.data))
		{
			GOTO_SET(&server_actions);
			if(args->next)
			{
				if(!(first_arg_value = sts_eval(script, args->next, locals, previous, 1, 0)))
				{
					fprintf(stderr, "could not eval first argument in task-create\n");
					return NULL;
				}
				else if(first_arg_value->type != STS_STRING)
				{
					fprintf(stderr, "first argument in task-create is not a string\n");
					return NULL;
				}


				if(!(temp_value = sts_value_create(script, STS_ARRAY)))
				{
					fprintf(stderr, "could not create temporary stash of arguments\n");
					return NULL;
				}

				args = args->next;

				while(args && (args = args->next))
				{
					if(!(eval_value = sts_eval(script, args, locals, previous, 1, 0)))
					{
						fprintf(stderr, "could not eval argument in task-create\n");
						sts_value_reference_decrement(script, temp_value);
						return NULL;
					}


					sts_array_append_insert(script, temp_value, eval_value, temp_value->array.length);
				}


				/* create a new thread for the task */

				if(!(task_args = calloc(1, sizeof(stsml_task_args_t))))
				{
					fprintf(stderr, "could not create task args\n");

					if(!sts_value_reference_decrement(script, temp_value))
						fprintf(stderr, "could not refdec the argument\n");
					
					if(!sts_value_reference_decrement(script, first_arg_value))
						fprintf(stderr, "could not refdec the argument\n");
						
					return NULL;
				}

				task_args->script_path = sts_memdup(first_arg_value->string.data, first_arg_value->string.length);

				if(!(task_args->args = sts_value_create(script, STS_NIL)))
				{
					fprintf(stderr, "could not create task arg array\n");

					if(!sts_value_reference_decrement(script, temp_value))
						fprintf(stderr, "could not refdec the argument\n");
					
					if(!sts_value_reference_decrement(script, first_arg_value))
						fprintf(stderr, "could not refdec the argument\n");
						
					return NULL;
				}


				/* copy args to prevent race conditions */


				if(sts_value_copy(script, task_args->args, temp_value, 1))
				{
					fprintf(stderr, "could not copy task arg array\n");

					if(!sts_value_reference_decrement(script, temp_value))
						fprintf(stderr, "could not refdec the argument\n");
					
					if(!sts_value_reference_decrement(script, first_arg_value))
						fprintf(stderr, "could not refdec the argument\n");
						
					return NULL;
				}


				/* create task thread */

				if(!(ret = sts_value_from_number(script, (double)pthread_create(&id, NULL, &start_task, (void *)task_args))))
				{
					fprintf(stderr, "could not create new ret number\n");

					if(!sts_value_reference_decrement(script, temp_value))
						fprintf(stderr, "could not refdec the argument\n");
					
					if(!sts_value_reference_decrement(script, first_arg_value))
						fprintf(stderr, "could not refdec the argument\n");
						
					return NULL;
				}
				

				if(!sts_value_reference_decrement(script, temp_value))
				{
					fprintf(stderr, "could not clean up task-create argument array\n");
					return NULL;
				}

				if(!sts_value_reference_decrement(script, first_arg_value))
				{
					fprintf(stderr, "could not clean up task-create first argument\n");
					return NULL;
				}

				/* === */
			}
			else
			{
				fprintf(stderr, "task-create requires at least 1 argument\n");
				return NULL;
			}
		}
	}

	if(!ret)
		ret = cli_actions(script, action, args, locals, previous);

	return ret;
}

char *stsml_import(sts_script_t *script, char *file)
{
	if(!strcmp(file, "stdlib.sts"))
	{
		#ifdef COMPILING
			return sts_memdup(lib_SimpleTinyScript_stdlib_sts, lib_SimpleTinyScript_stdlib_sts_len);
		#endif
	}
	return NULL;
}

int parse_args(stsml_args_t *args, int argc, char **argv)
{
	size_t i, j;


	for(i = 1; i < argc; i += 2)
	{
		if(!strcmp(argv[i] + 1, "help"))
		{
			printf("stsml server help\n\n");

			for(j = 0;; ++j)
			{
				if(!args[j].name) break;

				printf("'-%s:' %s\n", args[j].name, args[j].description);
			}

			printf("\n");

			return 1;
		}

		for(j = 0;; ++j)
		{
			if(!args[j].name) break;

			if(!strcmp(argv[i] + 1, args[j].name))
			{
				args[j].value = argv[i + 1];
				args[j].present = 1;
			}
		}
	}

	return 0;
}

char *get_arg_value(stsml_args_t *args, char *arg)
{
	size_t i;

	for(i = 0;; ++i)
	{
		if(!args[i].name) break;

		if(!strcmp(arg, args[i].name))
			return args[i].value;
	}

	return NULL;
}

int main(int argc, char **argv)
{
	unsigned long script_text_size = 0, offset = 0, line = 0;
	char *script_text = NULL;
	stsml_parser_ctx_t parser;
	stsml_ctx_t ctx;
	sts_value_t *res = NULL;
	sts_script_t script;
	sts_map_row_t *temp_row = NULL;
	onion_handler *stsml_handler = NULL, *router_handler = NULL, *file_handler = NULL, *last_resort_handler = NULL;
	stsml_script_t *temp_stript_locals = NULL;
	onion *on = NULL;
	stsml_args_t args[] = {
		{.name = "help", .description = "Prints this text.", .present = 0, .value = NULL},
		{.name = "init", .description = "Run a script on startup to setup global values and connections.", .present = 0, .value = NULL},
		{.name = "last_resort", .description = "Run a script when no script or file is found", .present = 0, .value = NULL},
		{.name = "port", .description = "Set the port to run on. By default, it's 8080.", .present = 0, .value = "8080"},
		{.name = "working_dir", .description = "Sets the working directory of stsml.", .present = 0, .value = NULL},
		{.name = NULL}
	};


	/* parse arguments */

	if(parse_args(args, argc, argv))
		return 0;

	if(get_arg_value(args, "working_dir"))
		chdir(get_arg_value(args, "working_dir"));


	ONION_INFO("starting stsml server");

	/* initialize the script */

	memset(&script, 0, sizeof(sts_script_t));

	/* set a read file callback */
	script.read_file = &read_file;

	/* initialize the router */
	script.router = &server_actions;

	script.import_file = &stsml_import;

	script.userdata = &ctx;


	/* initialize the stsml_ctx */

	memset(&ctx, 0, sizeof(stsml_ctx_t));

	ctx.parser = &parser;
	ctx.script = &script;
	ctx.last_resort = get_arg_value(args, "last_resort");

	if(!(script.globals = sts_scope_push(&script, NULL)))
	{
		ONION_ERROR("could not create global scope level");
		return 1;
	}


	/* initialize onion */

	if(!(on = onion_new(O_ONE_LOOP)))
	{
		ONION_ERROR("could not initialize onion");
		return 1;
	}

	/* if specified, run an initializer script */

	if(get_arg_value(args, "init"))
	{
		if(!(ctx.cleanup = sts_value_create(&script, STS_ARRAY)))
		{
			ONION_ERROR("could not initialize stsml ctx");
			onion_free(on);

			return 1;
		}

		if(!(script_text = read_file(&script, get_arg_value(args, "init"), &script_text_size)))
		{
			ONION_ERROR("could not initialize stsml ctx");
			onion_free(on);

			return 1;
		}

		/* parse */

		if(!(script.script = sts_parse(&script, NULL, script_text, get_arg_value(args, "init"), &offset, &line)))
		{
			ONION_ERROR("could not parse startup script at '%s'", get_arg_value(args, "init"));
			onion_free(on);
			free(script_text);
			
			return 1;
		}

		free(script_text);

		if(!(res = sts_eval(&script, script.script, NULL, NULL, 0, 0)))
		{
			ONION_ERROR("could not parse startup script at '%s'", get_arg_value(args, "init"));
			onion_free(on);
			sts_ast_delete(&script, script.script);
			free(script_text);
			
			return 1;
		}
		else
			sts_value_reference_decrement(&script, res);

		sts_ast_delete(&script, script.script);
		script.script = NULL;


		if(!sts_value_reference_decrement(&script, ctx.cleanup))
		{
			ONION_ERROR("could not refdec stsml ctx cleanup values");
			onion_free(on);
			
			return 1;
		}
	}

	/* initialize handlers */


	if(!(stsml_handler = onion_handler_new(&respond_stsml, &ctx, NULL)))
	{
		ONION_ERROR("could not initialize stsml handler");
		onion_free(on);

		return 1;
	}

	if(!(router_handler = onion_handler_new(&respond_index, &ctx, NULL)))
	{
		ONION_ERROR("could not initialize router handler");
		onion_free(on);

		return 1;
	}

	if(!(file_handler = onion_handler_new(&respond_file_specific, &ctx, NULL)))
	{
		ONION_ERROR("could not initialize file-specific handler");
		onion_free(on);

		return 1;
	}

	if(!(last_resort_handler = onion_handler_new(&respond_last_resort, &ctx, NULL)))
	{
		ONION_ERROR("could not initialize last resort handler");
		onion_free(on);

		return 1;
	}

	onion_set_hostname(on, "0.0.0.0");
	onion_set_port(on, get_arg_value(args, "port"));
	onion_set_max_threads(on, 0);

	onion_set_root_handler(on, router_handler);
	onion_handler_add(router_handler, file_handler);
	onion_handler_add(router_handler, stsml_handler);
	onion_handler_add(router_handler, last_resort_handler);

	onion_listen(on);


	/* ================================= */
	ONION_INFO("exitting...");

	if(onion_listen(on))
		ONION_ERROR("could not listen to incomming connections");


	/* destroy all locals */

	while(ctx.script_locals)
	{
		if((temp_stript_locals = ctx.script_locals->value))
		{
			if(!sts_destroy_map(&script, temp_stript_locals->locals->locals))
				ONION_ERROR("could not destroy local map for %d", ctx.script_locals->hash);
			
			free(temp_stript_locals->locals);
		}

		temp_row = ctx.script_locals;
		ctx.script_locals = ctx.script_locals->next;

		free(temp_row->value);
		free(temp_row);
	}


	sts_destroy(&script);

	if(ctx.redis_ctx) redisFree(ctx.redis_ctx);

	onion_free(on);


	ONION_INFO("goodbye");

	return 0;
}