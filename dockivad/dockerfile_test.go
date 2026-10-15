package main

import (
	"reflect"
	"testing"
)

func TestDockerfileParse(t *testing.T) {
	none := map[string]string{}
	for _, c := range []struct {
		name, text string
		escape     byte
		want       []instruction
	}{
		{"continuation", "FROM alpine\nRUN apk add \\\n    curl \\  \n  git\n", '\\', []instruction{
			{"FROM", none, "alpine", nil, "FROM alpine"},
			{"RUN", none, "apk add     curl   git", nil, "RUN apk add     curl   git"}}},
		{"comments and blanks in a continuation", "# top\n\nRUN a \\\n# comment \\\n\n   # indented\n b\n  # after\nCMD c", '\\', []instruction{
			{"RUN", none, "a  b", nil, "RUN a  b"},
			{"CMD", none, "c", nil, "CMD c"}}},
		{"CRLF and BOM", "\ufeffFROM alpine\r\nRUN a \\\r\n b\r\n", '\\', []instruction{
			{"FROM", none, "alpine", nil, "FROM alpine"},
			{"RUN", none, "a  b", nil, "RUN a  b"}}},
		{"escape directive", "# syntax=docker/dockerfile:1\n# Escape = `\nFROM windows\nRUN dir `\n c:\\\nWORKDIR c:\\x", '`', []instruction{
			{"FROM", none, "windows", nil, "FROM windows"},
			{"RUN", none, "dir  c:\\", nil, "RUN dir  c:\\"},
			{"WORKDIR", none, "c:\\x", nil, "WORKDIR c:\\x"}}},
		{"directive after a comment is a comment", "# hello\n# escape=`\nRUN a `\n", '\\', []instruction{
			{"RUN", none, "a `", nil, "RUN a `"}}},
		{"keyword case", "from alpine AS b\nRun\techo hi\nrun", '\\', []instruction{
			{"FROM", none, "alpine AS b", nil, "from alpine AS b"},
			{"RUN", none, "echo hi", nil, "Run\techo hi"},
			{"RUN", none, "", nil, "run"}}},
		{"flags", "FROM --platform=linux/amd64 alpine\nCOPY --from=builder --chmod=755 /a /b\n" +
			"RUN --mount=type=cache,target=/root/.cache pip install x\nADD --chown=a --chown=\"b c\" --link -- --x y\n" +
			"HEALTHCHECK --interval=5s CMD true\nCMD --help", '\\', []instruction{
			{"FROM", map[string]string{"platform": "linux/amd64"}, "alpine", nil, "FROM --platform=linux/amd64 alpine"},
			{"COPY", map[string]string{"from": "builder", "chmod": "755"}, "/a /b", nil, "COPY --from=builder --chmod=755 /a /b"},
			{"RUN", map[string]string{"mount": "type=cache,target=/root/.cache"}, "pip install x", nil, "RUN --mount=type=cache,target=/root/.cache pip install x"},
			{"ADD", map[string]string{"chown": "b c", "link": ""}, "--x y", nil, "ADD --chown=a --chown=\"b c\" --link -- --x y"},
			{"HEALTHCHECK", map[string]string{"interval": "5s"}, "CMD true", nil, "HEALTHCHECK --interval=5s CMD true"},
			{"CMD", none, "--help", nil, "CMD --help"}}},
		{"JSON and shell form", "CMD [\"a\", \\\n  \"b c\"]\nENTRYPOINT []\nRUN echo [hi]\nCMD [\"a\", \"b\"\nCOPY [\"a\" \"b\"] /c\nLABEL [\"a\", 1]", '\\', []instruction{
			{"CMD", none, `["a",   "b c"]`, []string{"a", "b c"}, `CMD ["a",   "b c"]`},
			{"ENTRYPOINT", none, "[]", []string{}, "ENTRYPOINT []"},
			{"RUN", none, "echo [hi]", nil, "RUN echo [hi]"},
			{"CMD", none, `["a", "b"`, nil, `CMD ["a", "b"`},
			{"COPY", none, `["a" "b"] /c`, nil, `COPY ["a" "b"] /c`},
			{"LABEL", none, `["a", 1]`, nil, `LABEL ["a", 1]`}}},
		{"not heredocs", "RUN cat <<< \"hi\"\nRUN echo \"<<EOF\" '<<EOF' \\<<EOF a<<EOF\nCMD cat <<EOF", '\\', []instruction{
			{"RUN", none, `cat <<< "hi"`, nil, `RUN cat <<< "hi"`},
			{"RUN", none, `echo "<<EOF" '<<EOF' \<<EOF a<<EOF`, nil, `RUN echo "<<EOF" '<<EOF' \<<EOF a<<EOF`},
			{"CMD", none, "cat <<EOF", nil, "CMD cat <<EOF"}}},
	} {
		t.Run(c.name, func(t *testing.T) {
			got, escape := parseDockerfile(c.text)
			if escape != c.escape || !reflect.DeepEqual(got, c.want) {
				t.Fatalf("got %q %#v\nwant %q %#v", escape, got, c.escape, c.want)
			}
		})
	}
	for _, bad := range []string{
		"# escape=x\nFROM a",
		"FROM a\nRUN <<EOF\necho hi\nEOF",
		"COPY <<EOF /x\nhi\nEOF",
		"RUN --mount=type=cache,target=/c cat <<-\"EOF\" > /x\nEOF",
		"ADD 3<<'EOF' /x",
		"ENTRYPOINT [\"a\", 1]",
		"VOLUME [[\"/a\"]]",
	} {
		t.Run(bad, func(t *testing.T) { mustReject(t, func() { parseDockerfile(bad) }) })
	}
}

func TestProcessWords(t *testing.T) {
	env := map[string]string{"A": "a", "E": "", "S": "x  y"}
	for _, c := range []struct {
		in     string
		split  bool
		escape byte
		want   []string
	}{
		{"$A ${A}b $Ab", false, '\\', []string{"a ab "}},
		{"${U:-d} ${E:-d} ${A:-d}", true, '\\', []string{"d", "d", "a"}},
		{"${U-d}|${E-d}|${A-d}", false, '\\', []string{"d||a"}},
		{"${U:+p}|${E:+p}|${A:+p}", false, '\\', []string{"||p"}},
		{"${U+p}|${E+p}|${A+p}", false, '\\', []string{"|p|p"}},
		{`${U:-${A}-"q  r"-'$A'} ${A:+${U:-z}}`, true, '\\', []string{"a-q  r-$A", "z"}},
		{"cost $5 $ a$ ${E:-}", false, '\\', []string{"cost $5 $ a$ "}},
		{`'$A "x" \'`, false, '\\', []string{`$A "x" \`}},
		{`"$A 'b' \$c \"d\" \\ \e ${S}"`, false, '\\', []string{`a 'b' $c "d" \ \e x  y`}},
		{`\$A \"q\" a\ b c\`, true, '\\', []string{"$A", `"q"`, "a b", "c"}},
		{"`$A \\x \"`\"\\\" a` b", true, '`', []string{"$A", `\x`, `"\`, "a b"}},
		{` a  b "c d" '' "" e$E	f `, true, '\\', []string{"a", "b", "c d", "", "", "e", "f"}},
		{"$S $U b", true, '\\', []string{"x  y", "b"}},
		{" a  b ", false, '\\', []string{" a  b "}},
		{"", true, '\\', []string{}},
		{"", false, '\\', []string{""}},
	} {
		t.Run(c.in, func(t *testing.T) {
			if got := processWords(c.in, env, c.escape, c.split); !reflect.DeepEqual(got, c.want) {
				t.Fatalf("got %q, want %q", got, c.want)
			}
		})
	}
	for _, bad := range []string{`"abc`, `'abc`, `"a\"`, "${A", "${A:-x", "${A:", "${}", "${1}", "${A?x}", `${A:-"}"`} {
		t.Run(bad, func(t *testing.T) { mustReject(t, func() { processWords(bad, env, '\\', true) }) })
	}
}
