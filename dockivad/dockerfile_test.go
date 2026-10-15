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
			{"FROM", none, "alpine", nil, "FROM alpine", nil, nil},
			{"RUN", none, "apk add     curl   git", nil, "RUN apk add     curl   git", nil, nil}}},
		{"comments and blanks in a continuation", "# top\n\nRUN a \\\n# comment \\\n\n   # indented\n b\n  # after\nCMD c", '\\', []instruction{
			{"RUN", none, "a  b", nil, "RUN a  b", nil, nil},
			{"CMD", none, "c", nil, "CMD c", nil, nil}}},
		{"CRLF and BOM", "\ufeffFROM alpine\r\nRUN a \\\r\n b\r\n", '\\', []instruction{
			{"FROM", none, "alpine", nil, "FROM alpine", nil, nil},
			{"RUN", none, "a  b", nil, "RUN a  b", nil, nil}}},
		{"escape directive", "# syntax=docker/dockerfile:1\n# Escape = `\nFROM windows\nRUN dir `\n c:\\\nWORKDIR c:\\x", '`', []instruction{
			{"FROM", none, "windows", nil, "FROM windows", nil, nil},
			{"RUN", none, "dir  c:\\", nil, "RUN dir  c:\\", nil, nil},
			{"WORKDIR", none, "c:\\x", nil, "WORKDIR c:\\x", nil, nil}}},
		{"directive after a comment is a comment", "# hello\n# escape=`\nRUN a `\n", '\\', []instruction{
			{"RUN", none, "a `", nil, "RUN a `", nil, nil}}},
		{"keyword case", "from alpine AS b\nRun\techo hi\nrun", '\\', []instruction{
			{"FROM", none, "alpine AS b", nil, "from alpine AS b", nil, nil},
			{"RUN", none, "echo hi", nil, "Run\techo hi", nil, nil},
			{"RUN", none, "", nil, "run", nil, nil}}},
		{"flags", "FROM --platform=linux/amd64 alpine\nCOPY --from=builder --chmod=755 /a /b\n" +
			"RUN --mount=type=cache,target=/root/.cache --network=none --mount=type=bind,target=. pip install x\nADD --chown=a --chown=\"b c\" --link -- --x y\n" +
			"HEALTHCHECK --interval=5s CMD true\nCMD --help", '\\', []instruction{
			{"FROM", map[string]string{"platform": "linux/amd64"}, "alpine", nil, "FROM --platform=linux/amd64 alpine", nil, nil},
			{"COPY", map[string]string{"from": "builder", "chmod": "755"}, "/a /b", nil, "COPY --from=builder --chmod=755 /a /b", nil, nil},
			{"RUN", map[string]string{"network": "none"}, "pip install x", nil, "RUN --mount=type=cache,target=/root/.cache --network=none --mount=type=bind,target=. pip install x",
				[]string{"type=cache,target=/root/.cache", "type=bind,target=."}, nil},
			{"ADD", map[string]string{"chown": "b c", "link": ""}, "--x y", nil, "ADD --chown=a --chown=\"b c\" --link -- --x y", nil, nil},
			{"HEALTHCHECK", map[string]string{"interval": "5s"}, "CMD true", nil, "HEALTHCHECK --interval=5s CMD true", nil, nil},
			{"CMD", none, "--help", nil, "CMD --help", nil, nil}}},
		{"JSON and shell form", "CMD [\"a\", \\\n  \"b c\"]\nENTRYPOINT []\nRUN echo [hi]\nCMD [\"a\", \"b\"\nCOPY [\"a\" \"b\"] /c\nLABEL [\"a\", 1]", '\\', []instruction{
			{"CMD", none, `["a",   "b c"]`, []string{"a", "b c"}, `CMD ["a",   "b c"]`, nil, nil},
			{"ENTRYPOINT", none, "[]", []string{}, "ENTRYPOINT []", nil, nil},
			{"RUN", none, "echo [hi]", nil, "RUN echo [hi]", nil, nil},
			{"CMD", none, `["a", "b"`, nil, `CMD ["a", "b"`, nil, nil},
			{"COPY", none, `["a" "b"] /c`, nil, `COPY ["a" "b"] /c`, nil, nil},
			{"LABEL", none, `["a", 1]`, nil, `LABEL ["a", 1]`, nil, nil}}},
		{"not heredocs", "RUN cat <<< \"hi\"\nRUN echo \"<<EOF\" '<<EOF' \\<<EOF a<<EOF\nCMD cat <<EOF", '\\', []instruction{
			{"RUN", none, `cat <<< "hi"`, nil, `RUN cat <<< "hi"`, nil, nil},
			{"RUN", none, `echo "<<EOF" '<<EOF' \<<EOF a<<EOF`, nil, `RUN echo "<<EOF" '<<EOF' \<<EOF a<<EOF`, nil, nil},
			{"CMD", none, "cat <<EOF", nil, "CMD cat <<EOF", nil, nil}}},
		{"heredocs", "# syntax = docker/dockerfile:1.7-labs\n# check=skip=all\nRUN <<EOF\n# not a comment\n\n  echo $A \\\nEOF\n" +
			"RUN --mount=type=cache,target=/c cat <<-\"EOF\" > /x && python3 <<'PY' \\\n  && true\n\tline\r\n\tEOF\nprint(1)\nPY\n" +
			"COPY <<A <<-B /dest/\na\nA\n\tb\n\tB\nCMD done\n", '\\', []instruction{
			{"RUN", none, "<<EOF", nil, "RUN <<EOF", nil, []heredoc{{"EOF", "# not a comment\n\n  echo $A \\\n", true, false}}},
			{"RUN", none, `cat <<-"EOF" > /x && python3 <<'PY'   && true`, nil, `RUN --mount=type=cache,target=/c cat <<-"EOF" > /x && python3 <<'PY'   && true`,
				[]string{"type=cache,target=/c"}, []heredoc{{"EOF", "\tline\r\n", false, true}, {"PY", "print(1)\n", false, false}}},
			{"COPY", none, "<<A <<-B /dest/", nil, "COPY <<A <<-B /dest/", nil, []heredoc{{"A", "a\n", true, false}, {"B", "\tb\n", true, true}}},
			{"CMD", none, "done", nil, "CMD done", nil, nil}}},
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
		"# syntax=example.com/my/frontend:1\nFROM a",
		"FROM a\nRUN <<EOF\necho hi\n",
		"COPY <<\"\" /x\n\n",
		"RUN <<-EOF\n EOF\n",
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
