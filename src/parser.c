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

#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

enum internal_flags
{
	PARSER_IMPORT_RELATIVE = 1,
	PARSER_IMPORT_CWD = (1<<1),
	PARSER_DIRECTIVE_STANDARD = (1<<2),
	PARSER_DIRECTIVE_PRINT = (1<<3)
};


int stsml_parser_init(stsml_parser_ctx_t *ctx)
{
	memset(ctx, 0, sizeof(stsml_parser_ctx_t));

	return 0;
}

int stsml_parser_run(stsml_parser_ctx_t *ctx, char *input, char *pwd)
{
	char *temp_str = NULL, *escaped_str, *partial_string = NULL, *import_file = NULL, *new_cwd = NULL;
	stsml_parser_ctx_t new_ctx;
	unsigned int flags = 0;


	if(!ctx->pos)
	{
		ctx->pos = input;
		ctx->start = input;
	}
	
	do
	{
		if(ctx->flags & STSML_PARSER_STRING_LITERAL)
		{
			if(ctx->pos[0] == '\"' && ctx->pos[-1] != '\\')
				ctx->flags ^= STSML_PARSER_STRING_LITERAL;
		}
		else
		{
			if(ctx->pos[0] == '<' && ctx->pos[1] == '%' && (ctx->pos[2] == '!' || ctx->pos[2] == '@')) /* include (cwd or relative) */
			{
				if((ctx->pos - ctx->start) > 0)
				{
					if(!(temp_str = strndup(ctx->start, ctx->pos - ctx->start)))
					{
						fprintf(stderr, "could not duplicate string\n");
						
						if(ctx->assembled)
							free(ctx->assembled);

						return 1;
					}

					if(!(escaped_str = stsml_parser_escape(temp_str)))
					{
						fprintf(stderr, "could not escape string\n");
						
						if(ctx->assembled)
							free(ctx->assembled);
						
						free(temp_str);

						return 1;
					}

					stsml_asprintf(&ctx->assembled, "%s\nhttp-write \"%s\"", ctx->assembled ? ctx->assembled : "", escaped_str);

					free(temp_str);
					free(escaped_str);

					temp_str = NULL;
				}
				/* continue onto the actual importing */

				flags = 0;
				if(ctx->pos[2] == '!')
					flags |= PARSER_IMPORT_CWD;
				else
					flags |= PARSER_IMPORT_RELATIVE;


				/* skip past <%X */

				ctx->pos += 3;
				
				/* loop past whitespace */
				while(*(ctx->pos) && isspace(*(ctx->pos))) ++ctx->pos;

				ctx->start = ctx->pos;

				/* fast forward to first unescaped space or %> */

				while(*(ctx->pos))
				{
					while(*(ctx->pos) && (!isspace(*ctx->pos) && !(ctx->pos[0] == '%' && ctx->pos[1] == '>')) ) ++ctx->pos;

					if(ctx->pos[-1] != '\\')
					{
						if(!(partial_string = strndup(ctx->start, ctx->pos - ctx->start)))
						{
							fprintf(stderr, "could not copy string before escape\n");

							return 1;
						}
						
						stsml_asprintf(&temp_str, "%s%s", (temp_str ? temp_str : ""), partial_string);

						free(partial_string);

						break;
					}

					if(!(partial_string = strndup(ctx->start, (ctx->pos - 1) - ctx->start)))
					{
						fprintf(stderr, "could not copy string before escape\n");

						return 1;
					}
					
					stsml_asprintf(&temp_str, "%s%s", (temp_str) ? temp_str : "", partial_string);

					free(partial_string);

					ctx->start = ctx->pos;
					//ctx->start = ctx->pos + 1;
					ctx->pos++;
				}

				/* in case it has not been reached, fastforward to end */
				if(!(ctx->pos[0] == '%' && ctx->pos[1] == '>') && ctx->pos[0])
					while(!(ctx->pos[0] == '%' && ctx->pos[1] == '>') && ctx->pos[0]) ++ctx->pos;


				/* create new parser ctx and use the returned string */
				fprintf(stderr, "including '%s'%s\n", temp_str, (flags & PARSER_IMPORT_RELATIVE) ? " relative to current stsml file" : "");


				stsml_parser_init(&new_ctx);

				if(flags & PARSER_IMPORT_RELATIVE)
				{
					stsml_asprintf(&temp_str, "%s%s", pwd, temp_str);
				}

				if(!(import_file = stsml_parser_read_file(temp_str, NULL)))
				{
					fprintf(stderr, "could not read file '%s\n", temp_str);
					free(temp_str);

					return 1;
				}

				if(!(new_cwd = stsml_parser_pwd_from_file(temp_str)))
				{
					fprintf(stderr, "could not create new pwd for file '%s\n", temp_str);

					free(temp_str);
					free(import_file);

					return 1;
				}

				if(stsml_parser_run(&new_ctx, import_file, new_cwd))
				{
					fprintf(stderr, "could not parse included file '%s\n", temp_str);

					free(temp_str);
					free(import_file);
					free(new_cwd);

					return 1;
				}

				/* finally add the included parsed script to the current script */
				stsml_asprintf(&ctx->assembled, "%s\n%s", ctx->assembled ? ctx->assembled : "", new_ctx.assembled ? new_ctx.assembled : "");


				free(new_ctx.assembled);
				free(temp_str);
				free(import_file);
				free(new_cwd);

				ctx->start = ctx->pos + 2;

			}
			else if(ctx->pos[0] == '<' && ctx->pos[1] == '%' && ctx->pos[2] == '?') /* print expression */
			{
				if((ctx->pos - ctx->start) > 0)
				{
					if(!(temp_str = strndup(ctx->start, ctx->pos - ctx->start)))
					{
						fprintf(stderr, "could not duplicate string\n");
						
						if(ctx->assembled)
							free(ctx->assembled);

						return 1;
					}

					if(!(escaped_str = stsml_parser_escape(temp_str)))
					{
						fprintf(stderr, "could not escape string\n");
						
						if(ctx->assembled)
							free(ctx->assembled);
						
						free(temp_str);

						return 1;
					}

					stsml_asprintf(&ctx->assembled, "%s\nhttp-write \"%s\"", ctx->assembled ? ctx->assembled : "", escaped_str);

					free(temp_str);
					free(escaped_str);

					temp_str = NULL;
				}

				flags |= PARSER_DIRECTIVE_PRINT;

				ctx->start = ctx->pos + 3;
			}
			/* check if start of script or EOF */
			else if(((ctx->pos[0] == '<' && ctx->pos[1] == '%') || !ctx->pos[0]) && (ctx->pos - ctx->start) > 0)
			{
				flags |= PARSER_DIRECTIVE_STANDARD;
				if(!(temp_str = strndup(ctx->start, ctx->pos - ctx->start)))
				{
					fprintf(stderr, "could not duplicate string\n");
					
					if(ctx->assembled)
						free(ctx->assembled);

					return 1;
				}

				if(!(escaped_str = stsml_parser_escape(temp_str)))
				{
					fprintf(stderr, "could not escape string\n");
					
					if(ctx->assembled)
						free(ctx->assembled);
					
					free(temp_str);

					return 1;
				}

				stsml_asprintf(&ctx->assembled, "%s\nhttp-write \"%s\"", ctx->assembled ? ctx->assembled : "", escaped_str);

				free(temp_str);
				free(escaped_str);

				/* start new script part */
				ctx->start = ctx->pos + 2;
			}
			else if(ctx->pos[0] == '<' && ctx->pos[1] == '%') /* for the case when it starts at the very beginning of the file */
			{
				flags |= PARSER_DIRECTIVE_STANDARD;

				ctx->start = ctx->pos + 2;
			}
			else if(ctx->pos[0] == '%' && ctx->pos[1] == '>') /* duplicate inline script */
			{
				if(!(temp_str = strndup(ctx->start, ctx->pos - ctx->start)))
				{
					fprintf(stderr, "could not duplicate string\n");
					
					if(ctx->assembled)
						free(ctx->assembled);

					return 1;
				}

				if(flags & PARSER_DIRECTIVE_STANDARD)
					stsml_asprintf(&ctx->assembled, "%s\n%s", ctx->assembled ? ctx->assembled : "", temp_str);
				else
					stsml_asprintf(&ctx->assembled, "%s\nhttp-write [string (%s)]", ctx->assembled ? ctx->assembled : "", temp_str);

				free(temp_str);

				flags = 0;

				/* start new document print string */
				ctx->start = ctx->pos + 2;
			}
			else if(ctx->pos[0] == '\"')
			{
				ctx->flags |= STSML_PARSER_STRING_LITERAL;
			}
		}

		/* set back to null */
		temp_str = NULL;
		escaped_str = NULL;
		partial_string = NULL;
		import_file = NULL;
		new_cwd = NULL;

	} while((*ctx->pos && ++ctx->pos));


	return 0;
}

/* escapes double quotes */
char *stsml_parser_escape(char *string)
{
	char *ret = NULL, *start = string, old = 0;
	unsigned int i, strsize = strlen(string);


	for(i = 0; i <= strsize; ++i)
	{
		//if(string[i] == '\"' || string[i] == '\\' || string[i] == '\r' || string[i] == '\n')
		if(string[i] == '\"' || string[i] == '\\')
		{
			old = string[i];
			string[i] = 0x0;

			stsml_asprintf(&ret, "%s%s\\", ret ? ret : "", start);

			string[i] = old;

			start = &string[i];
		}
		else if(string[i] == 0x0)
		{
			stsml_asprintf(&ret, "%s%s", ret ? ret : "", start);
		}
	}

	return ret;
}

char *stsml_parser_read_file(char *path, unsigned int *size)
{
	char *ret = NULL;
	FILE *file = NULL;
	unsigned int file_size = 0;


	if(!(file = fopen(path, "r")))
	{
		fprintf(stderr, "could not open file at path: %s\n", path);
		return NULL;
	}

	fseek(file, 0, SEEK_END);
	file_size = ftell(file);
	fseek(file, 0, SEEK_SET);


	if(!(ret = calloc(1, file_size + 1)))
	{
		fprintf(stderr, "could not allocate memory for file from: %s\n", path);
		fclose(file);
		return NULL;
	}

	if(fread(ret, file_size, 1, file) <= 0)
	{
		fprintf(stderr, "could not read file data for file: %s\n", path);
		free(ret);
		fclose(file);
		return NULL;
	}

	if(size)
		*size = file_size;
	
	fclose(file);

	return ret;
}

short stsml_asprintf(char **string, const char *fmt, ...)
{
	va_list list;
	char *temp_string = NULL;
	char *oldstring = NULL;
	int size = 0;

	if(*string != NULL)
	{
		//free(*string);
    	oldstring = *string;
	}

	va_start(list, fmt);
	size = vsnprintf(temp_string, 0, fmt, list);
	va_end(list);
	va_start(list, fmt);

	if((temp_string = malloc(size + 1)) != NULL)
	{
    	if(vsnprintf(temp_string, size + 1, fmt, list) != -1)
    	{
    		*string = temp_string;
    		if(oldstring != NULL)
			{
				free(oldstring);
			}
    	return size;
    }
    else
    {
		*string = NULL;
		if(oldstring != NULL)
		{
			free(oldstring);
		}
			return -1;
		}
	}
	va_end(list);

	return size;
}

char *stsml_parser_pwd_from_file(char *path)
{
	char *ret = NULL;


	if(strrchr(path, '/'))
		ret = strndup(path, strrchr(path, '/') - path + 1);
	else
		ret = calloc(1, 1);

	return ret;
}