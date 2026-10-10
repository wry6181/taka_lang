CC = cc
CFLAGS = -Wall -Wextra -g
SRC = lexer.c parser.c arena.c ast_build.c ast_dump.c sema.c cgen.c x64.c
HDR = lexer.h types.h arena.h ast.h ast_build.h sema.h cgen.h x64.h taka_runtime.h ds_list.h ds_map.h ds_set.h

all: taka

taka: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o taka

demo.gen.c: taka demo.tk
	./taka demo.tk

demo: demo.gen.c taka_runtime.h
	$(CC) $(CFLAGS) demo.gen.c -o demo

mut.gen.c: taka mut.tk
	./taka mut.tk

ds_test: ds_test.c arena.c ds_list.h ds_map.h ds_set.h
	$(CC) $(CFLAGS) ds_test.c arena.c -o ds_test

enum_test: taka enum_test.tk taka_runtime.h
	./taka enum_test.tk
	$(CC) $(CFLAGS) enum_test.gen.c -o enum_test

scope_test: taka scope_test.tk taka_runtime.h
	./taka scope_test.tk
	$(CC) $(CFLAGS) scope_test.gen.c -o scope_test

list_test: taka list_test.tk taka_runtime.h
	./taka list_test.tk
	$(CC) $(CFLAGS) list_test.gen.c -o list_test

str_test: taka str_test.tk taka_runtime.h
	./taka str_test.tk
	$(CC) $(CFLAGS) str_test.gen.c -o str_test

map_test: taka map_test.tk taka_runtime.h
	./taka map_test.tk
	$(CC) $(CFLAGS) map_test.gen.c -o map_test

file_fixture.txt:
	printf 'AB' > file_fixture.txt

file_test: taka file_test.tk taka_runtime.h file_fixture.txt
	./taka file_test.tk
	$(CC) $(CFLAGS) file_test.gen.c -o file_test

args_test: taka args_test.tk taka_runtime.h
	./taka args_test.tk
	$(CC) $(CFLAGS) args_test.gen.c -o args_test

test: taka ds_test file_fixture.txt
	./ds_test
	./taka build demo.tk -o /tmp/taka-build-check
	/tmp/taka-build-check
	rm -f /tmp/taka-build-check
	./taka mut.tk
	./taka syntax_test.tk
	./taka demo.tk
	$(CC) $(CFLAGS) demo.gen.c -o demo
	./demo
	./taka enum_test.tk
	$(CC) $(CFLAGS) enum_test.gen.c -o enum_test
	./enum_test
	./taka scope_test.tk
	$(CC) $(CFLAGS) scope_test.gen.c -o scope_test
	./scope_test
	./taka list_test.tk
	$(CC) $(CFLAGS) list_test.gen.c -o list_test
	./list_test
	./taka str_test.tk
	$(CC) $(CFLAGS) str_test.gen.c -o str_test
	./str_test
	./taka map_test.tk
	$(CC) $(CFLAGS) map_test.gen.c -o map_test
	./map_test
	./taka file_test.tk
	$(CC) $(CFLAGS) file_test.gen.c -o file_test
	./file_test
	./taka args_test.tk
	$(CC) $(CFLAGS) args_test.gen.c -o args_test
	./args_test hello

# differential: native backend must match C backend on every test program
test-native: taka
	for t in demo enum_test scope_test list_test str_test file_test map_test torture; do \
		./taka run $$t.tk > /tmp/taka-c-$$t.txt 2>&1 || exit 1; \
		./taka --backend=native $$t.tk >/dev/null 2>&1 || exit 1; \
		$(CC) $(CFLAGS) $$t.s -o /tmp/taka-n-$$t || exit 1; \
		/tmp/taka-n-$$t > /tmp/taka-n-$$t.txt 2>&1 || exit 1; \
		diff /tmp/taka-c-$$t.txt /tmp/taka-n-$$t.txt || exit 1; \
		rm -f $$t.s; \
	done
	./taka --backend=native args_test.tk >/dev/null 2>&1 || exit 1
	$(CC) $(CFLAGS) args_test.s -o /tmp/taka-n-args || exit 1
	printf '2\n1\n1\n0\n' > /tmp/taka-n-args.want
	/tmp/taka-n-args hello > /tmp/taka-n-args.txt 2>&1 || exit 1
	diff /tmp/taka-n-args.want /tmp/taka-n-args.txt || exit 1
	rm -f args_test.s

# stage-1 self-host check: taka lexer must match the C lexer byte-for-byte
lexdiff: taka
	$(CC) $(CFLAGS) lexdump.c lexer.c -o /tmp/taka-lexdump
	./taka build self_lex.tk -o /tmp/taka-selflex-c
	./taka --backend=native build self_lex.tk -o /tmp/taka-selflex-n
	for f in *.tk; do \
		/tmp/taka-lexdump "$$f" > /tmp/taka-lex-c.txt 2>&1 || exit 1; \
		/tmp/taka-selflex-c "$$f" > /tmp/taka-lex-tc.txt 2>&1 || exit 1; \
		/tmp/taka-selflex-n "$$f" > /tmp/taka-lex-tn.txt 2>&1 || exit 1; \
		diff /tmp/taka-lex-c.txt /tmp/taka-lex-tc.txt || exit 1; \
		diff /tmp/taka-lex-c.txt /tmp/taka-lex-tn.txt || exit 1; \
	done
	rm -f /tmp/taka-lexdump /tmp/taka-selflex-c /tmp/taka-selflex-n

# stage-2 self-host check: taka parser must match the C builder byte-for-byte
sexpdiff: taka
	./taka build self_parse.tk -o /tmp/taka-selfparse-c || exit 1
	./taka --backend=native build self_parse.tk -o /tmp/taka-selfparse-n || exit 1
	for f in *.tk; do \
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
	./taka build self_parse.tk -o /tmp/taka-selfcheck-c || exit 1
	./taka --backend=native build self_parse.tk -o /tmp/taka-selfcheck-n || exit 1
	for f in *.tk; do \
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
	./taka --backend=native build self_parse.tk -o /tmp/taka-stage-parse || exit 1
	./taka --backend=native build self_emit.tk -o /tmp/taka-stage-emit || exit 1
	/tmp/taka-stage-parse demo.tk > /tmp/taka-sh.sexp || exit 1
	/tmp/taka-stage-emit /tmp/taka-sh.sexp /tmp/taka-sh.s || exit 1
	cc /tmp/taka-sh.s -o /tmp/taka-sh-bin || exit 1
	/tmp/taka-sh-bin > /tmp/taka-sh-out.txt || exit 1
	printf '30\n10\n0\n' > /tmp/taka-sh.want
	diff /tmp/taka-sh.want /tmp/taka-sh-out.txt || exit 1
	rm -f /tmp/taka-stage-parse /tmp/taka-stage-emit

# stage-4 self-host check: taka emitter must match C x64_emit byte-for-byte
emitdiff: taka file_fixture.txt
	./taka build self_emit.tk -o /tmp/taka-selfemit-c || exit 1
	./taka --backend=native build self_emit.tk -o /tmp/taka-selfemit-n || exit 1
	for f in demo enum_test scope_test list_test str_test file_test map_test args_test torture; do \
		./taka sexp $$f.tk > /tmp/taka-sx.txt 2>&1 || exit 1; \
		./taka --backend=native $$f.tk >/dev/null 2>&1 || exit 1; \
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
	cd editors/tree-sitter-taka && for f in ../../*.tk; do \
		n=$$(tree-sitter parse "$$f" 2>&1 | grep -cE "ERROR|MISSING"); \
		echo "$$f errors:$$n"; test "$$n" = "0" || exit 1; \
	done

clean:
	rm -f taka demo demo.gen.c ds_test enum_test enum_test.gen.c scope_test scope_test.gen.c list_test list_test.gen.c str_test str_test.gen.c str_out.txt map_test map_test.gen.c file_test file_test.gen.c file_fixture.txt args_test args_test.gen.c

.PHONY: all test test-native run-demo clean release zed-sync test-zed
