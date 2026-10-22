"""Private-copy test hack: --fragment-library FILE takes the fragment function from a second metallib."""
p = '/opt/work/m2v/crates/metal2vulkan/src/main.rs'
s = open(p).read()
def sub(old, new):
    global s
    assert s.count(old) == 1, old[:60]
    s = s.replace(old, new)
sub("    fragment: Option<String>,\n", "    fragment: Option<String>,\n    fragment_library: Option<String>,\n")
sub('            "--fragment" => parsed.fragment = Some(value()),\n',
    '            "--fragment" => parsed.fragment = Some(value()),\n            "--fragment-library" => parsed.fragment_library = Some(value()),\n')
sub("""            if let Some(name) = &args.fragment {
                let fragment = library
                    .function(name)""",
    """            let fragment_library_owned;
            let fragment_library = match &args.fragment_library {
                Some(path) => {
                    fragment_library_owned = Library::parse(&std::fs::read(path).expect("fragment library"))
                        .expect("fragment library parse");
                    &fragment_library_owned
                }
                None => library,
            };
            if let Some(name) = &args.fragment {
                let fragment = fragment_library
                    .function(name)""")
open(p, 'w').write(s)
print('patched')
