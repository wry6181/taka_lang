CC = cc
CFLAGS = -Wall -Wextra -g
SRC = lexer.c parser.c arena.c ast_build.c sema.c cgen.c
HDR = lexer.h types.h arena.h ast.h ast_build.h sema.h cgen.h taka_runtime.h ds_list.h ds_map.h ds_set.h

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
	rm -f taka demo demo.gen.c ds_test enum_test enum_test.gen.c scope_test scope_test.gen.c list_test list_test.gen.c str_test str_test.gen.c str_out.txt map_test map_test.gen.c file_test file_test.gen.c file_fixture.txt

.PHONY: all test run-demo clean release zed-sync test-zed
