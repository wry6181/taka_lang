CC = cc
CFLAGS = -Wall -Wextra -g
# include dir for generated C (runtime/taka_runtime.h)
GENINC = -Iruntime
SRC = src/lexer.c src/parser.c src/arena.c src/ast_build.c src/ast_dump.c src/sema.c src/cgen.c src/x64.c
HDR = src/lexer.h src/types.h src/arena.h src/ast.h src/ast_build.h src/sema.h src/cgen.h src/x64.h runtime/taka_runtime.h src/ds_list.h src/ds_map.h src/ds_set.h

all: taka

taka: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o taka

demo.gen.c: taka tests/demo.tk
	./taka tests/demo.tk

demo: demo.gen.c runtime/taka_runtime.h
	$(CC) $(CFLAGS) $(GENINC) demo.gen.c -o demo

mut.gen.c: taka tests/mut.tk
	./taka tests/mut.tk

ds_test: tools/ds_test.c src/arena.c src/ds_list.h src/ds_map.h src/ds_set.h
	$(CC) $(CFLAGS) -Isrc tools/ds_test.c src/arena.c -o ds_test

enum_test: taka tests/enum_test.tk runtime/taka_runtime.h
	./taka tests/enum_test.tk
	$(CC) $(CFLAGS) $(GENINC) enum_test.gen.c -o enum_test

scope_test: taka tests/scope_test.tk runtime/taka_runtime.h
	./taka tests/scope_test.tk
	$(CC) $(CFLAGS) $(GENINC) scope_test.gen.c -o scope_test

list_test: taka tests/list_test.tk runtime/taka_runtime.h
	./taka tests/list_test.tk
	$(CC) $(CFLAGS) $(GENINC) list_test.gen.c -o list_test

str_test: taka tests/str_test.tk runtime/taka_runtime.h
	./taka tests/str_test.tk
	$(CC) $(CFLAGS) $(GENINC) str_test.gen.c -o str_test

map_test: taka tests/map_test.tk runtime/taka_runtime.h
	./taka tests/map_test.tk
	$(CC) $(CFLAGS) $(GENINC) map_test.gen.c -o map_test

tests/file_fixture.txt:
	printf 'AB' > tests/file_fixture.txt

file_test: taka tests/file_test.tk runtime/taka_runtime.h tests/file_fixture.txt
	./taka tests/file_test.tk
	$(CC) $(CFLAGS) $(GENINC) file_test.gen.c -o file_test

args_test: taka tests/args_test.tk runtime/taka_runtime.h
	./taka tests/args_test.tk
	$(CC) $(CFLAGS) $(GENINC) args_test.gen.c -o args_test

test: taka ds_test tests/file_fixture.txt
	./ds_test
	./taka build tests/demo.tk -o /tmp/taka-build-check
	/tmp/taka-build-check
	rm -f /tmp/taka-build-check
	./taka tests/mut.tk
	./taka tests/syntax_test.tk
	./taka tests/demo.tk
	$(CC) $(CFLAGS) $(GENINC) demo.gen.c -o demo
	./demo
	./taka tests/enum_test.tk
	$(CC) $(CFLAGS) $(GENINC) enum_test.gen.c -o enum_test
	./enum_test
	./taka tests/scope_test.tk
	$(CC) $(CFLAGS) $(GENINC) scope_test.gen.c -o scope_test
	./scope_test
	./taka tests/list_test.tk
	$(CC) $(CFLAGS) $(GENINC) list_test.gen.c -o list_test
	./list_test
	./taka tests/str_test.tk
	$(CC) $(CFLAGS) $(GENINC) str_test.gen.c -o str_test
	./str_test
	./taka tests/map_test.tk
	$(CC) $(CFLAGS) $(GENINC) map_test.gen.c -o map_test
	./map_test
	./taka tests/file_test.tk
	$(CC) $(CFLAGS) $(GENINC) file_test.gen.c -o file_test
	./file_test
	./taka tests/args_test.tk
	$(CC) $(CFLAGS) $(GENINC) args_test.gen.c -o args_test
	./args_test hello

# differential: native backend must match C backend on every test program
test-native: taka
	for t in demo enum_test scope_test list_test str_test file_test map_test torture; do \
		./taka run tests/$$t.tk > /tmp/taka-c-$$t.txt 2>&1 || exit 1; \
		./taka --backend=native tests/$$t.tk >/dev/null 2>&1 || exit 1; \
		$(CC) $(CFLAGS) $$t.s -o /tmp/taka-n-$$t || exit 1; \
		/tmp/taka-n-$$t > /tmp/taka-n-$$t.txt 2>&1 || exit 1; \
		diff /tmp/taka-c-$$t.txt /tmp/taka-n-$$t.txt || exit 1; \
		rm -f $$t.s; \
	done
	./taka --backend=native tests/args_test.tk >/dev/null 2>&1 || exit 1
	$(CC) $(CFLAGS) args_test.s -o /tmp/taka-n-args || exit 1
	printf '2\n1\n1\n0\n' > /tmp/taka-n-args.want
	/tmp/taka-n-args hello > /tmp/taka-n-args.txt 2>&1 || exit 1
	diff /tmp/taka-n-args.want /tmp/taka-n-args.txt || exit 1
	rm -f args_test.s

# stage-1 self-host check: taka lexer must match the C lexer byte-for-byte
lexdiff: taka
	$(CC) $(CFLAGS) -Isrc tools/lexdump.c src/lexer.c -o /tmp/taka-lexdump
	./taka build selfhost/self_lex.tk -o /tmp/taka-selflex-c
	./taka --backend=native build selfhost/self_lex.tk -o /tmp/taka-selflex-n
	for f in tests/*.tk selfhost/*.tk; do \
		/tmp/taka-lexdump "$$f" > /tmp/taka-lex-c.txt 2>&1 || exit 1; \
		/tmp/taka-selflex-c "$$f" > /tmp/taka-lex-tc.txt 2>&1 || exit 1; \
		/tmp/taka-selflex-n "$$f" > /tmp/taka-lex-tn.txt 2>&1 || exit 1; \
		diff /tmp/taka-lex-c.txt /tmp/taka-lex-tc.txt || exit 1; \
		diff /tmp/taka-lex-c.txt /tmp/taka-lex-tn.txt || exit 1; \
	done
	rm -f /tmp/taka-lexdump /tmp/taka-selflex-c /tmp/taka-selflex-n

# stage-2 self-host check: taka parser must match the C builder byte-for-byte
sexpdiff: taka
	./taka build selfhost/self_parse.tk -o /tmp/taka-selfparse-c || exit 1
	./taka --backend=native build selfhost/self_parse.tk -o /tmp/taka-selfparse-n || exit 1
	for f in tests/*.tk selfhost/*.tk; do \
		./taka sexp $$f > /tmp/taka-sx-c.txt 2>&1; \
		test $$? -le 1 || exit 1; \
		/tmp/taka-selfparse-c $$f > /tmp/taka-sx-tc.txt 2>&1; \
		test $$? -le 1 || exit 1; \
		/tmp/taka-selfparse-n $$f > /tmp/taka-sx-tn.txt 2>&1; \
		test $$? -le 1 || exit 1; \
		diff /tmp/taka-sx-c.txt /tmp/taka-sx-tc.txt || exit 1; \
		diff /tmp/taka-sx-c.txt /tmp/taka-sx-tn.txt || exit 1; \
	done
	rm -f /tmp/taka-selfparse-c /tmp/taka-selfparse-n

# stage-3 self-host check: taka checker must match C sema (verdict+messages)
checkdiff: taka
	./taka build selfhost/self_parse.tk -o /tmp/taka-selfcheck-c || exit 1
	./taka --backend=native build selfhost/self_parse.tk -o /tmp/taka-selfcheck-n || exit 1
	for f in tests/*.tk selfhost/*.tk; do \
		./taka check $$f > /tmp/taka-ck-c.txt 2>&1; \
		test $$? -le 1 || exit 1; \
		/tmp/taka-selfcheck-c $$f --check > /tmp/taka-ck-tc.txt 2>&1; \
		test $$? -le 1 || exit 1; \
		/tmp/taka-selfcheck-n $$f --check > /tmp/taka-ck-tn.txt 2>&1; \
		test $$? -le 1 || exit 1; \
		diff /tmp/taka-ck-c.txt /tmp/taka-ck-tc.txt || exit 1; \
		diff /tmp/taka-ck-c.txt /tmp/taka-ck-tn.txt || exit 1; \
	done
	rm -f /tmp/taka-selfcheck-c /tmp/taka-selfcheck-n

# full self-host closure: .tk -> binary using only self-hosted stages.
# Stage binaries are built natively (no C involved); cc is only the assembler.
selfhost: taka
	./taka --backend=native build selfhost/self_parse.tk -o /tmp/taka-stage-parse || exit 1
	./taka --backend=native build selfhost/self_emit.tk -o /tmp/taka-stage-emit || exit 1
	/tmp/taka-stage-parse tests/demo.tk > /tmp/taka-sh.sexp || exit 1
	/tmp/taka-stage-emit /tmp/taka-sh.sexp /tmp/taka-sh.s || exit 1
	cc /tmp/taka-sh.s -o /tmp/taka-sh-bin || exit 1
	/tmp/taka-sh-bin > /tmp/taka-sh-out.txt || exit 1
	printf '30\n10\n0\n' > /tmp/taka-sh.want
	diff /tmp/taka-sh.want /tmp/taka-sh-out.txt || exit 1
	rm -f /tmp/taka-stage-parse /tmp/taka-stage-emit

# stage-4 self-host check: taka emitter must match C x64_emit byte-for-byte
emitdiff: taka tests/file_fixture.txt
	./taka build selfhost/self_emit.tk -o /tmp/taka-selfemit-c || exit 1
	./taka --backend=native build selfhost/self_emit.tk -o /tmp/taka-selfemit-n || exit 1
	for f in demo enum_test scope_test list_test str_test file_test map_test args_test torture; do \
		./taka sexp tests/$$f.tk > /tmp/taka-sx.txt 2>&1 || exit 1; \
		./taka --backend=native tests/$$f.tk >/dev/null 2>&1 || exit 1; \
		mv $$f.s /tmp/taka-emit-ref.s; \
		/tmp/taka-selfemit-c /tmp/taka-sx.txt /tmp/taka-emit-tc.s || exit 1; \
		/tmp/taka-selfemit-n /tmp/taka-sx.txt /tmp/taka-emit-tn.s || exit 1; \
		diff /tmp/taka-emit-ref.s /tmp/taka-emit-tc.s || exit 1; \
		diff /tmp/taka-emit-ref.s /tmp/taka-emit-tn.s || exit 1; \
		cc /tmp/taka-emit-tc.s -o /tmp/taka-emit-run || exit 1; \
		cc /tmp/taka-emit-ref.s -o /tmp/taka-emit-ref || exit 1; \
		/tmp/taka-emit-run > /tmp/taka-emit-out.txt 2>&1 || exit 1; \
		/tmp/taka-emit-ref > /tmp/taka-emit-ref.txt 2>&1 || exit 1; \
		diff /tmp/taka-emit-out.txt /tmp/taka-emit-ref.txt || exit 1; \
	done
	rm -f /tmp/taka-selfemit-c /tmp/taka-selfemit-n

run-demo: demo
	./demo

release: CFLAGS += -O2
release: taka

zed-sync:
	cp editors/tree-sitter-taka/queries/*.scm editors/zed/languages/taka/

test-zed:
	command -v tree-sitter >/dev/null || (echo "need tree-sitter CLI" && exit 1)
	cd editors/tree-sitter-taka && tree-sitter generate
	cd editors/tree-sitter-taka && python3 check_queries.py
	cd editors/tree-sitter-taka && for f in ../../tests/*.tk ../../selfhost/*.tk; do \
		n=$$(tree-sitter parse "$$f" 2>&1 | grep -cE "ERROR|MISSING"); \
		echo "$$f errors:$$n"; test "$$n" = "0" || exit 1; \
	done

clean:
	rm -f taka demo demo.gen.c ds_test enum_test enum_test.gen.c scope_test scope_test.gen.c list_test list_test.gen.c str_test str_test.gen.c map_test map_test.gen.c file_test file_test.gen.c args_test args_test.gen.c torture torture.gen.c mut.gen.c lexdump
	rm -f tests/file_fixture.txt tests/str_out.txt
	rm -f *.s

.PHONY: all test test-native run-demo clean release zed-sync test-zed
