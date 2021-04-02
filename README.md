# STSML
STS Markup Language can do simple scriptable html modifications with the scripting language [STS](https://github.com/zeroxthreef/SimpleTinyScript) (simple tiny script). Sort of like of PHP but this is nowhere near that level.

stsml has an asynchronus task system and redis client functions built in.


## STSML Documentation
stsml is different from vanilla STS's syntax in the following examples: (note that any amount of space including none is ok for these)


``<% ... %>`` (multiline expression)<br>
Multiline STS expressions can be done inside matching brackets. If the file only contains these, nothing will be sent unless explicitly written to the buffer.

Example:
```
<%
	print this message will go directly to stderr

	print hello :)
%>
<html>
	<body>
		<% local i 0 %>

		<% loop(< $i 10) { %>

			<b>hello, currently at: <%? pass $i %></b>

		<% ++ $i; }  # the semicolon is used to put multiple expressions on the same line %>
	</body>
</html>
```


``<%! file/path %>`` (include absolute)<br>
``<%@ file/path %>`` (include relative)<br>
stsml files can be included directly into the current document relative to the current path '@', or relative to the server's working directory '!'. Note that a recursive include will either stack overflow or oom your machine.



``<%? ... %>``(print single expression string value)<br>
Print directly to the current http buffer using just a single STS expression. Note that this means you have to pass a root expression, which means that if you arent calling a function, a `pass` action needs to be used.

Example:
```
<%
	local test "hello world"
%>
<html>
	<body>
		<h1>local value: <%? pass $test %>
		<b><%? string a b c d e f %></b>
	</body>
</html>
```


## STSML Function Documentation

`http-write append_string`<br>
Append strings to the current http buffer.

`http-clear`<br>
Clear the http buffer.

`http-method-get`<br>
Returns the current http method as a string like 'GET'.

`http-path-get`<br>
Returns the current http not-query string aka the request path

`http-body-get`<br>
Returns the raw http body.

`http-post-get post_key_str`<br>
Returns post data as a string if found, nil if not.

`http-query-get query_key_str`<br>
Returns url query data as a string if found, nil if not.

`http-file-get post_file_name_str`<br>
Returns a string of the file uploaded to a path somewhere in /tmp/ if exists, nil if not. Note that this temporary file disappears when the script exits.

`http-cookie-get cookie_key_str`<br>
Returns the cookie value if found, nil if not.

`http-cookie-put cookie_key_str cookie_value_str cookie_ttl cookie_flags`<br>
Sets a cookie. Flags are 1 for httponly and 2 for secure. Bitwise OR both of these and secure httponly cookies will be set.

`http-header-get header_key_str`<br>
Returns the header value string if found, nil if not.

`http-header-put header_key_str header_value_str`<br>
Set an http header to the header value string.

`http-write-file path_str`<br>
Write a file to the http buffer instead. This is loaded after scripts finish and overrides the regular buffer.

`http-route absolute_str`<br>
Reroute the server internally. Path is absolute and the root is the working directory.

`redis-connect ip_str port_number`<br>
Connects to a Redis server and returns 1.0 on a successful connection, 0.0 otherwise.

`redis ...`<br>
Sends a Redis command to the global connection. Will only take strings and numbers. Any other value type will be skipped. Note that the total number (not size of arguments, these can be nearly infinite) of arguments is limited to an already absurdly long amount of arguments **(1024 max)**, but nothing bad will happen if this is reached, which is near impossible.

This returns a value that converts the redis response to an STS value, so it may be an array, string, or any other kind of value.

`task-create script_file ...`<br>
Create a task thread for asynchronus things. Can run forever if necessary. **Note that these do not share globals with the rest of the system** and all arguments passed are recursively copied.


## Server Usage
`-help`<br>
Print help page that lists this section.

`-port`<br>
Set the http port. The default is 8080.

`-init`<br>
Set the startup STS script. **NOTE: this is NOT FOR STSML SCRIPTS. Only regular STS scripts will work**. All of the same functions will work, but it will not be parsed as an stsml file.

`-last_resort`<br>
Display an stsml file if everything else fails. Note that this will halt the server if this file is not found.

`-working_dir`<br>
Set the server working directory.


## Building & Installing
stsml depends on [hiredis](https://github.com/redis/hiredis) and [onion](https://github.com/davidmoreno/onion).

**Debian/Ubuntu:**<br>
```
sudo apt install libhiredis-dev
git clone https://github.com/davidmoreno/onion.git
cd onion
... complete onion install instructions
cd ..
git clone https://github.com/zeroxthreef/stsml.git --recursive
cd stsml
./build.sh
sudo ./install.sh
```


## License
Public Domain / Unlicense (pretty much the same)