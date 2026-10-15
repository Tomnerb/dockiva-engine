package main

import (
	"encoding/json"
	"regexp"
	"strings"
)

// instruction is one Dockerfile instruction.
type instruction struct {
	cmd      string            // upper-case keyword: FROM, RUN, COPY, ...
	flags    map[string]string // --name=value options before the arguments (FROM, COPY, ADD, RUN, HEALTHCHECK); never nil
	rest     string            // the arguments after the flags, as written (continuations joined)
	json     []string          // the JSON (exec) form of the arguments when rest is a valid JSON array of strings; nil otherwise
	original string            // the whole instruction as written, continuations joined, for "Step 1/5 : ..." output
	mounts   []string          // RUN's --mount values, in order (the flag repeats)
	heredocs []heredoc         // RUN, COPY and ADD here-documents (<<EOF), in order
}

// heredoc is a here-document (Dockerfile 1.4 syntax): the lines after the
// instruction up to the delimiter.
type heredoc struct {
	name   string // the delimiter, quotes removed
	body   string // the lines, each with its newline, as written
	expand bool   // an unquoted delimiter: COPY and ADD expand variables in the body
	chomp  bool   // <<-: leading tabs are dropped, from the delimiter line too
}

// heredocFor is the here-document a COPY or ADD source (<<EOF) names, or nil.
func (in instruction) heredocFor(src string) *heredoc {
	if m := heredocWord.FindStringSubmatch(src); m != nil {
		for i := range in.heredocs {
			if in.heredocs[i].name == m[2] {
				return &in.heredocs[i]
			}
		}
	}
	return nil
}

// content is the file a COPY or ADD here-document makes, or a RUN's script.
func (h heredoc) content() string {
	if !h.chomp {
		return h.body
	}
	lines := strings.SplitAfter(h.body, "\n")
	for i, l := range lines {
		lines[i] = strings.TrimLeft(l, "\t")
	}
	return strings.Join(lines, "")
}

var directivePattern = regexp.MustCompile(`^#\s*([a-zA-Z][a-zA-Z0-9]*)\s*=\s*(.+?)\s*$`)

// A heredoc is a word <<WORD or <<-WORD (not <<<), quotes and escapes left in, as
// BuildKit's parser sees it. A quoted WORD keeps the body from expansion.
var heredocWord = regexp.MustCompile(`^\d*<<(-?)([^<]*)$`)

// The docker/dockerfile frontends (# syntax=docker/dockerfile:1.7 and the like):
// their dialect is the builder's own.
var dockerfileSyntax = regexp.MustCompile(`^(docker\.io/)?docker/dockerfile(-upstream)?(:[A-Za-z0-9._-]+)?(@sha256:[0-9a-f]{64})?$`)

var varName = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*`)

// parseDockerfile parses a Dockerfile as Docker's classic builder does. It returns
// the instructions and the escape character (\ unless an escape directive sets `).
func parseDockerfile(text string) ([]instruction, byte) {
	escape := byte('\\')
	directives := true
	out := []instruction{}
	lines := strings.Split(strings.TrimPrefix(text, "\ufeff"), "\n")
	for i := 0; i < len(lines); i++ {
		line := strings.TrimLeft(strings.TrimRight(lines[i], "\r"), " \t")
		if directives {
			m, name := directivePattern.FindStringSubmatch(line), ""
			if m != nil {
				name = strings.ToLower(m[1])
			}
			switch name {
			case "escape":
				reject(m[2] == `\` || m[2] == "`", "Invalid escape directive: "+m[2])
				escape = m[2][0]
			case "syntax":
				reject(dockerfileSyntax.MatchString(m[2]), "# syntax="+m[2]+": Dockiva Engine builds with its own Dockerfile frontend, which reads the docker/dockerfile syntax only")
			case "check":
			default:
				directives = false
			}
		}
		if line == "" || line[0] == '#' {
			continue
		}
		// Comment and blank lines inside a continuation are dropped, and the
		// lines are joined as written (no newline, indentation kept), as Docker does.
		joined, more := continued(line, escape)
		for more && i+1 < len(lines) {
			i++
			next := strings.TrimRight(lines[i], "\r")
			if t := strings.TrimLeft(next, " \t"); t == "" || t[0] == '#' {
				continue
			}
			next, more = continued(next, escape)
			joined += next
		}
		if strings.TrimSpace(joined) == "" {
			continue
		}
		in := newInstruction(joined)
		for k := range in.heredocs { // each body follows the instruction, in order
			h := &in.heredocs[k]
			for {
				i++
				reject(i < len(lines), "Unterminated heredoc <<"+h.name+" in: "+in.original)
				end := strings.TrimRight(lines[i], "\r")
				if h.chomp {
					end = strings.TrimLeft(end, "\t")
				}
				if end == h.name {
					break
				}
				h.body += lines[i] + "\n"
			}
		}
		out = append(out, in)
	}
	return out, escape
}

// continued drops a trailing escape character, reporting whether the next line continues this one.
func continued(line string, escape byte) (string, bool) {
	t := strings.TrimRight(line, " \t")
	if strings.HasSuffix(t, string(rune(escape))) {
		return t[:len(t)-1], true
	}
	return line, false
}

// newInstruction splits one joined line into its keyword, flags and arguments.
func newInstruction(line string) instruction {
	line = strings.TrimSpace(line)
	end := strings.IndexAny(line, " \t")
	if end < 0 {
		end = len(line)
	}
	in := instruction{cmd: strings.ToUpper(line[:end]), flags: map[string]string{}, original: line}
	rest := line[end:]
	switch in.cmd {
	case "FROM", "COPY", "ADD", "RUN", "HEALTHCHECK":
		for {
			rest = strings.TrimLeft(rest, " \t")
			if !strings.HasPrefix(rest, "--") {
				break
			}
			var word string
			if word, rest = flagWord(rest); word == "--" {
				break
			}
			name, value, _ := strings.Cut(word[2:], "=")
			if in.cmd == "RUN" && name == "mount" {
				in.mounts = append(in.mounts, value)
				continue
			}
			in.flags[name] = value
		}
	}
	in.rest = strings.TrimSpace(rest)
	if in.cmd == "RUN" || in.cmd == "COPY" || in.cmd == "ADD" {
		for _, w := range rawWords(in.rest) {
			if m := heredocWord.FindStringSubmatch(w); m != nil && m[2] != "" {
				name, _ := flagWord(m[2])
				reject(name != "", "Invalid heredoc: "+w)
				in.heredocs = append(in.heredocs, heredoc{name: name, expand: name == m[2], chomp: m[1] == "-"})
			}
		}
	}
	var array []any
	if strings.HasPrefix(in.rest, "[") && json.Unmarshal([]byte(in.rest), &array) == nil {
		in.json = []string{}
		for _, v := range array {
			s, ok := v.(string)
			if !ok {
				// Docker refuses, rather than runs as shell form, a non-string array where exec form is allowed.
				reject(!strings.Contains(" RUN CMD ENTRYPOINT SHELL COPY ADD VOLUME ", " "+in.cmd+" "),
					"When using JSON array syntax, arrays must hold only strings: "+line)
				in.json = nil
				break
			}
			in.json = append(in.json, s)
		}
	}
	return in
}

// flagWord splits the first blank-separated word off s, removing quotes and \
// escapes as Docker does for instruction flags.
func flagWord(s string) (string, string) {
	var b strings.Builder
	var quote byte
	for i := 0; i < len(s); i++ {
		switch c := s[i]; {
		case quote == 0 && (c == ' ' || c == '\t'):
			return b.String(), s[i:]
		case quote != 0 && c == quote:
			quote = 0
		case quote == 0 && (c == '\'' || c == '"'):
			quote = c
		case c == '\\':
			if i+1 < len(s) {
				i++
				b.WriteByte(s[i])
			}
		default:
			b.WriteByte(c)
		}
	}
	return b.String(), ""
}

// rawWords splits s on unquoted blanks, keeping quotes and escapes.
func rawWords(s string) []string {
	out := []string{}
	var b strings.Builder
	var quote byte
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case quote == 0 && (c == ' ' || c == '\t'):
			if b.Len() > 0 {
				out = append(out, b.String())
				b.Reset()
			}
			continue
		case quote != '\'' && c == '\\' && i+1 < len(s):
			b.WriteByte(c)
			i++
			c = s[i]
		case quote == 0 && (c == '\'' || c == '"'):
			quote = c
		case c == quote:
			quote = 0
		}
		b.WriteByte(c)
	}
	if b.Len() > 0 {
		out = append(out, b.String())
	}
	return out
}

// expandHeredoc expands variables in a here-document as a shell does: quotes are
// text, and \ escapes only $, \ and a newline.
func expandHeredoc(s string, env map[string]string) string {
	l := lexer{s: s, env: env, escape: '\\'}
	var b strings.Builder
	for l.i < len(l.s) {
		c := l.s[l.i]
		l.i++
		switch {
		case c == '$':
			b.WriteString(l.dollar())
		case c == '\\' && l.i < len(l.s) && strings.IndexByte("$\\\n", l.s[l.i]) >= 0:
			if l.s[l.i] != '\n' {
				b.WriteByte(l.s[l.i])
			}
			l.i++
		default:
			b.WriteByte(c)
		}
	}
	return b.String()
}

// processWords expands variables and removes quotes like Docker's builder does for
// ENV, ARG, LABEL, COPY, WORKDIR and the like. With split it returns the words
// (split on unquoted blanks); without, it returns exactly one word: the whole input.
func processWords(s string, env map[string]string, escape byte, split bool) []string {
	l := lexer{s: s, env: env, escape: escape}
	return l.words(0, split)
}

// lexer reads one processWords input.
type lexer struct {
	s      string
	i      int
	env    map[string]string
	escape byte
}

// words reads to the end of the input or, when stop is not 0, past the next
// unquoted stop. A variable's value is never split into words, as Docker's ENV,
// LABEL and COPY split the arguments before expanding them.
// ponytail: EXPOSE $PORTS with PORTS="80 443" yields one word; Docker splits it there, so split that result on blanks.
func (l *lexer) words(stop byte, split bool) []string {
	out := []string{}
	var b strings.Builder
	started := !split // without split there is always exactly one word
	for {
		if l.i == len(l.s) {
			reject(stop == 0, "Missing '}' in: "+l.s)
			break
		}
		c := l.s[l.i]
		l.i++
		if stop != 0 && c == stop {
			break
		}
		switch {
		case split && (c == ' ' || c == '\t'):
			if started {
				out = append(out, b.String())
				b.Reset()
			}
			started = false
			continue
		case c == '\'':
			end := strings.IndexByte(l.s[l.i:], '\'')
			reject(end >= 0, "Unterminated quote in: "+l.s)
			b.WriteString(l.s[l.i : l.i+end])
			l.i += end + 1
		case c == '"':
			l.quoted(&b)
		case c == '$':
			v := l.dollar()
			if v == "" {
				continue // an empty unquoted expansion makes no word
			}
			b.WriteString(v)
		case c == l.escape:
			if l.i == len(l.s) {
				continue // Docker drops a trailing escape character
			}
			b.WriteByte(l.s[l.i])
			l.i++
		default:
			b.WriteByte(c)
		}
		started = true
	}
	if started {
		out = append(out, b.String())
	}
	return out
}

// quoted reads a double-quoted string after its opening quote: variables expand,
// and the escape character escapes only ", $, itself and a newline.
func (l *lexer) quoted(b *strings.Builder) {
	for {
		reject(l.i < len(l.s), "Unterminated quote in: "+l.s)
		c := l.s[l.i]
		l.i++
		switch {
		case c == '"':
			return
		case c == '$':
			b.WriteString(l.dollar())
		case c == l.escape && l.i < len(l.s) && (l.s[l.i] == l.escape || strings.IndexByte("\"$\n", l.s[l.i]) >= 0):
			b.WriteByte(l.s[l.i])
			l.i++
		default:
			b.WriteByte(c)
		}
	}
}

// dollar expands the variable after a $ (already read); a $ without a name stays "$".
// ponytail: $1, $@, $$ stay literal (Docker expands them to ""), and ${NAME?word},
// ${NAME#...}, ${NAME%...}, ${NAME/...} are refused; add them when a Dockerfile needs them.
func (l *lexer) dollar() string {
	brace := l.i < len(l.s) && l.s[l.i] == '{'
	if brace {
		l.i++
	}
	name := varName.FindString(l.s[l.i:])
	l.i += len(name)
	value, set := l.env[name]
	if !brace {
		if name == "" {
			return "$"
		}
		return value
	}
	op := l.next()
	reject(name != "", "Bad substitution in: "+l.s)
	if op == '}' {
		return value
	}
	if op == ':' {
		op = l.next()
		set = set && value != ""
	}
	reject(op == '-' || op == '+', "Unsupported substitution in: "+l.s)
	word := l.words('}', false)[0]
	switch {
	case op == '-' && set:
		return value
	case op == '-' || set:
		return word
	}
	return ""
}

// next reads one byte inside ${...}.
func (l *lexer) next() byte {
	reject(l.i < len(l.s), "Missing '}' in: "+l.s)
	l.i++
	return l.s[l.i-1]
}
