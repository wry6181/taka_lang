CC = cc
CFLAGS = -Wall -Wextra -g
SRC = lexer.c parser.c arena.c ast_build.c sema.c cgen.c x64.c
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
