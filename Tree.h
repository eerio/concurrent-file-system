#pragma once

// Kod błędu zwracany przy próbie przeniesienia folderu do swojego podfolderu
#define EINVMV (-20)

// Let "Tree" mean the same as "struct Tree".
typedef struct Tree Tree;

// Tworzy nowe drzewo folderów z jednym, pustym folderem "/".
Tree* tree_new();

// Zwalnia całą pamięć związaną z podanym drzewem.
void tree_free(Tree*);

// Wymienia zawartość danego folderu, zwracając nowy napis postaci "foo,bar,baz"
char* tree_list(Tree* tree, const char* path);

// Tworzy nowy podfolder (np. dla path="/foo/bar/baz/", tworzy pusty podfolder baz w folderze "/foo/bar/").
int tree_create(Tree* tree, const char* path);

// Usuwa folder, o ile jest pusty.
int tree_remove(Tree* tree, const char* path);

// Przenosi folder source wraz z zawartością na miejsce target (przenoszone jest całe poddrzewo), o ile to możliwe 
int tree_move(Tree* tree, const char* source, const char* target);
