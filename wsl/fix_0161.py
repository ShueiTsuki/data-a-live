def patch(p, subs):
    s = open(p).read()
    for o, n in subs:
        assert s.count(o) == 1, (p, o[:60])
        s = s.replace(o, n)
    open(p, 'w').write(s)


# files.txt was written on Windows with CRLF line ends, so every path kept a trailing '\r':
# the installer created "main.caml\r" and "Arm.png\r" next to the real files, and the
# wallpaper's main.caml kept pointing at the HEIC images. Line ends are now stripped.
patch('bsd/dev/pandora_files.c', [
    ("""		next = strchr(line, '\\n');
		if (next) {
			*next++ = 0;
		}
""",
     """		next = strchr(line, '\\n');
		if (next) {
			*next++ = 0;
		}
		for (size_t len = strlen(line); len && (line[len - 1] == '\\r' || line[len - 1] == ' '); len--) {
			line[len - 1] = 0;
		}
"""),
])
print('patched')
