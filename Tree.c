#include <errno.h>
#include <stdlib.h>
#include <stddef.h> // NULL
#include <string.h> // strlen
#include <assert.h>
#include <pthread.h>

#include "Tree.h"
#include "HashMap.h"
#include "err.h"
#include "path_utils.h"
#include "rwlock.h"

struct Tree {
  HashMap *hmap;
  rwlock_t *rwlock;
};

Tree* tree_new() {
  Tree *tree = (Tree *)malloc(sizeof(Tree));
  if (!tree) { bad_malloc(); }
  if (!(tree->rwlock = rwlock_new())) { syserr("Unable to create lock"); }
  if (!(tree->hmap = hmap_new())) { bad_malloc(); }
  return tree;
}

// Można zakładać, że operacja tree_free zostanie wykonana na danym drzewie dokładnie raz, po zakończeniu wszystkich innych operacji.
// wiec nie musimy blokowac wierzcholkow, caller musi poczekac az sie skoncza
void tree_free(Tree* tree) {
  const char *key;
  void *value;
  HashMapIterator it = hmap_iterator(tree->hmap);
  while (hmap_next(tree->hmap, &it, &key, &value)) {
    Tree *child = (Tree *)value;
    tree_free(child);
  }

  rwlock_destroy(tree->rwlock);
  hmap_free(tree->hmap);
  free(tree);
  return;
}

typedef enum TraverseMode {
  WEAK,
  LOCK,
  UNLOCK
} TraverseMode;

Tree *path_rdunlock(Tree *tree, const char *path) {
  if (!tree) { return NULL; }
  assert(path && *path);

  Tree *result = tree;
  Tree *subtree = tree;
  char component[MAX_FOLDER_NAME_LENGTH + 1];
  const char *subpath = path;
  if ((subpath = split_path(subpath, component))) {
    assert(subtree);
    subtree = (Tree *)hmap_get(subtree->hmap, component);
    result = path_rdunlock(subtree, subpath);
    rwlock_rdunlock(tree->rwlock);
    if (!subtree) { return NULL; }
  }

  return result;
}

// ta funkcja jest kluczowa - zdobywa read-locki na wierzcholkach na sciezce
// od korzenia do wierzcholka pod *path, a potem zwraca szukany folder (niezablokwany)
Tree *get_subfolder(Tree *tree, const char *path, TraverseMode mode) {
  assert(path);
  if (mode == UNLOCK) { return path_rdunlock(tree, path); }

  Tree *subtree = tree;

  char component[MAX_FOLDER_NAME_LENGTH + 1];
  const char *subpath = path;
  while ((subpath = split_path(subpath, component))) {
    if (mode == LOCK) { rwlock_rdlock(subtree->rwlock); }

    subtree = (Tree *)hmap_get(subtree->hmap, component);

    if (!subtree) { return NULL; }
  }

  return subtree;
}

char* tree_list(Tree* tree, const char *path) {
  if (!is_path_valid(path)) { return NULL; }

  Tree *subtree = get_subfolder(tree, path, LOCK);
  if (!subtree) {
    assert(get_subfolder(tree, path, UNLOCK) == subtree);
    return NULL;
  }

  rwlock_rdlock(subtree->rwlock);
  char *result = make_map_contents_string(subtree->hmap);
  rwlock_rdunlock(subtree->rwlock);

  assert(get_subfolder(tree, path, UNLOCK) == subtree);
  return result;
}

int tree_create(Tree* tree, const char* path) {
  if (!is_path_valid(path)) { return EINVAL; }
  if (!strcmp(path, "/")) { return EEXIST; }
  
  char component[MAX_FOLDER_NAME_LENGTH + 1];
  char *parent_path = make_path_to_parent(path, component);
  Tree *subtree = get_subfolder(tree, parent_path, LOCK);
  if (!subtree) { assert(!get_subfolder(tree, parent_path, UNLOCK)); free(parent_path); return ENOENT; }

  Tree *new_node = tree_new();
  rwlock_wrlock(subtree->rwlock);
  bool insert_successful = hmap_insert(subtree->hmap, component, new_node);
  rwlock_wrunlock(subtree->rwlock);

  assert(get_subfolder(tree, parent_path, UNLOCK) == subtree);
  free(parent_path);
  
  if (!insert_successful) {
    tree_free(new_node);
    return EEXIST;
  }
  return 0;
}

int tree_remove(Tree* tree, const char* path) {
  if (!is_path_valid(path)) { return EINVAL; }
  if (!strcmp(path, "/")) { return EBUSY; }

  int result = 0;

  char component[MAX_FOLDER_NAME_LENGTH + 1];
  char *parent_path = make_path_to_parent(path, component);
  Tree *parent = get_subfolder(tree, parent_path, LOCK);
  if (!parent) { result = ENOENT; goto exit1; }

  rwlock_wrlock(parent->rwlock);
  // we have read-write permissions, so no operation is running in the subtree

  Tree *node = (Tree *)hmap_get(parent->hmap, component);
  if (!node) { result = ENOENT; goto exit2; }
  if (hmap_size(node->hmap)) { result = ENOTEMPTY; goto exit2; }

  assert(hmap_remove(parent->hmap, component));
  tree_free(node);

exit2:
  rwlock_wrunlock(parent->rwlock);
exit1:
  assert(get_subfolder(tree, parent_path, UNLOCK) == parent);
  free(parent_path);
  return result;
}

// returns true if str starts with prefix and is longer, false otherwise
bool starts_with(const char *str, const char *prefix) {
  return strlen(str) > strlen(prefix) && (strncmp(str, prefix, strlen(prefix)) == 0);
}

Tree *get_lca(Tree *tree, const char* source, const char* target, TraverseMode mode) {
  // get longest common prefix of source and target
  const char *prefix_end1 = source, *prefix_end2 = target;
  while (*prefix_end1 && *prefix_end2 && *prefix_end1 == *prefix_end2) { prefix_end1++; prefix_end2++; }

  // find path of lca
  const char *last_slash = source;
  for (const char *c=source; c < prefix_end1; ++c) {
    if (*c == '/') { last_slash = c; }
  }
  char lca_path[MAX_PATH_LENGTH + 1];
  strncpy(lca_path, source, last_slash - source + 1);
  lca_path[last_slash - source + 1] = '\0';

  return get_subfolder(tree, lca_path, mode);
}

/*

Opis synchronizacji:
Schodząc wgłąd drzewa, na każdym stopniu zbieram rwlocki w trybie czytelnika
To zabezpiecza mnie przed przeniesieniem folderu, na którym aktualnie pracuję
i dziwnymi przeplotami. W każdej z funkcji wyżej zbieram tylko jednego locka
w trybie pisarza, więc tam nie ma zadnych kłopotów z zakleszczeniami - tutaj
jest inaczej. Żeby rozwiązać ten problem, zamiast blokować osobno dwa wierzchołki
(próby tego szybszego rozwiązania są poniżej), od razu blokuję w trybie pisarza
LCA ojców szukanych wierzchołków. Dzięki temu blokujemy tylko jeden wierzchołek,
co zabezpiecza nas przed deadlockami. Ponadto dzięki trybowi pisarza, możemy
dowoli czytać i pisać w całym poddrzewie, zatem pozostałe operacje wykonuejmy
w trybie WEAK, tj. bez zbierania żadnych locków. Locki oddajemy w kolejności
odwrotnej niż je zbieraliśmy, co robimy za pomocą post-order rekurencji w funkcji
path_rdunlock
*/
int tree_move(Tree *tree, const char *source, const char *target) {
  if (!source || !is_path_valid(source)) { return EINVAL; }
  if (!target || !is_path_valid(target)) { return EINVAL; }
  if (!strcmp(source, "/")) { return EBUSY; }
  if (!strcmp(target, "/")) { return EEXIST; }

  char source_component[MAX_FOLDER_NAME_LENGTH + 1];
  char *source_parent_path = make_path_to_parent(source, source_component);

  char target_component[MAX_FOLDER_NAME_LENGTH + 1];
  char *target_parent_path = make_path_to_parent(target, target_component);

  int result = 0;
  if (starts_with(target, source)) { result = EINVMV; goto exit0; }
  if (starts_with(source, target)) {
    Tree *node = get_subfolder(tree, source, LOCK);
    assert(get_subfolder(tree, source, UNLOCK) == node);
    result = node ? EEXIST : ENOENT;
    goto exit0;
  }
  
  Tree *lca = get_lca(tree, source_parent_path, target_parent_path, LOCK);
  if (!lca) { result = ENOENT; goto exit1; }

  rwlock_wrlock(lca->rwlock);
  
  Tree *source_parent = get_subfolder(tree, source_parent_path, WEAK);
  if (!source_parent) { result = ENOENT; goto exit2; }
  
  Tree *target_parent = get_subfolder(tree, target_parent_path, WEAK);
  if (!target_parent) { result = ENOENT; goto exit2; }
  
  Tree *source_node = hmap_get(source_parent->hmap, source_component);
  if (!source_node) { result = ENOENT; goto exit2; }
  
  assert(hmap_remove(source_parent->hmap, source_component));
  bool success = hmap_insert(target_parent->hmap, target_component, source_node);
  if (!success) {
    assert(hmap_insert(source_parent->hmap, source_component, source_node));
    result = EEXIST;
  }

exit2:
  rwlock_wrunlock(lca->rwlock);
exit1:
  assert(get_lca(tree, source_parent_path, target_parent_path, UNLOCK) == lca);
exit0:
  free(source_parent_path); 
  free(target_parent_path);

  return result;
}



// tutaj ponizej jest tylko do wgladu owoc mojej dluugiej pracy, niestety
// nie dziala to




// to jest wersja, ktora nie blokuje LCA; ona dzialala
// (tj. nie generowala deadlockow i byla poprawna) przy zastosowaniu
// rwlocka z pthreads; z moją implementacja rwlocka niestety się kleszczy
int tree_moveSEMI(Tree *tree, const char *source, const char *target) {
  if (!source || !is_path_valid(source)) { return EINVAL; }
  if (!target || !is_path_valid(target)) { return EINVAL; }
  if (!strcmp(source, "/")) { return EBUSY; }
  if (!strcmp(target, "/")) { return EEXIST; }
  
  char source_component[MAX_FOLDER_NAME_LENGTH + 1];
  char *source_parent_path = make_path_to_parent(source, source_component);

  char target_component[MAX_FOLDER_NAME_LENGTH + 1];
  char *target_parent_path = make_path_to_parent(target, target_component);

  int result = 0;
  if (starts_with(target, source)) { result = EINVMV; goto exit0; }
  if (starts_with(source, target)) {
    Tree *node = get_subfolder(tree, source, LOCK);
    assert(get_subfolder(tree, source, UNLOCK) == node);
    result = node ? EEXIST : ENOENT;
    goto exit0;
  }
  Tree *lca = get_lca(tree, source_parent_path, target_parent_path, LOCK);
  if (!lca) { result = ENOENT; goto exit1; }

  int cmp = strcmp(source_parent_path, target_parent_path);
  cmp = cmp ? cmp / abs(cmp) : 0;

  TraverseMode mode;

  if (!cmp || starts_with(source_parent_path, target_parent_path) || starts_with(target_parent_path, source_parent_path) ) {
    rwlock_wrlock(lca->rwlock);
    mode = WEAK;
  } else {
    mode = LOCK;
  }
  
  // DEADLOCK JEST TU
  // TRZEBA WCHODZIC WGŁĄB A NIE WZDLUZ TJ BFSEM TO SZUKAMY
  // ale to roziwaze w sumie w ogole problem?

  // Tree *source_parent, *target_parent;
  // get_two_subfolders(tree, source_parent_path, target_parent_path, mode);
  // if (!source_parent || !target_parent) { result = ENOENT; goto exit2; }

  Tree *source_parent, *target_parent;

  // Tree *source_parent = get_subfolder(tree, source_parent_path, mode);
  // if (!source_parent) { result = ENOENT; goto exit2; }
  
  // Tree *target_parent = get_subfolder(tree, target_parent_path, mode);
  // if (!target_parent) { result = ENOENT; goto exit3; }

  // if (source_parent > target_parent) { cmp = 1; }
  // else if (source_parent < target_parent) { cmp = -1; }
  // else { cmp = 0; }

  if (mode == LOCK) {
    if (cmp == -1) {
      source_parent = get_subfolder(tree, source_parent_path, mode);
      if (!source_parent) { result = ENOENT; goto exit2; }
      rwlock_wrlock(source_parent->rwlock);

      target_parent = get_subfolder(tree, target_parent_path, mode);
      if (!target_parent) { result = ENOENT; goto exit3; }
      rwlock_wrlock(target_parent->rwlock);
    } else if (cmp == 1) {
      target_parent = get_subfolder(tree, target_parent_path, mode);
      if (!target_parent) { result = ENOENT; goto exit2; }
      rwlock_wrlock(target_parent->rwlock);

      source_parent = get_subfolder(tree, source_parent_path, mode);
      if (!source_parent) { result = ENOENT; goto exit3; }
      rwlock_wrlock(source_parent->rwlock);
    } else {
      fatal("cannot happen");
    }
  } else {
    source_parent = get_subfolder(tree, source_parent_path, mode);
    if (!source_parent) { result = ENOENT; goto exit2; }
    
    target_parent = get_subfolder(tree, target_parent_path, mode);
    if (!target_parent) { result = ENOENT; goto exit3; }
  }
  
  Tree *source_node = hmap_get(source_parent->hmap, source_component);
  if (!source_node) { result = ENOENT; goto exit4; }
  
  assert(hmap_remove(source_parent->hmap, source_component));
  bool success = hmap_insert(target_parent->hmap, target_component, source_node);
  if (!success) {
    assert(hmap_insert(source_parent->hmap, source_component, source_node));
    result = EEXIST;
  }

exit4:
  if (mode == LOCK) {
    if (cmp == -1) {
      rwlock_wrunlock(target_parent->rwlock);
      // rwlock_wrunlock(source_parent->rwlock);
    } else if (cmp == 1) {
      rwlock_wrunlock(source_parent->rwlock);
      // rwlock_wrunlock(target_parent->rwlock);
    }
  }
exit3:
  if (mode == LOCK) {
    if (cmp == -1) {
      // rwlock_wrunlock(target_parent->rwlock);
      rwlock_wrunlock(source_parent->rwlock);
    } else if (cmp == 1) {
      // rwlock_wrunlock(source_parent->rwlock);
      rwlock_wrunlock(target_parent->rwlock);
    }
  }
  if (mode == LOCK) {
    assert (cmp == 1 || cmp == -1);
    if (cmp == -1) {
      assert(get_subfolder(tree, target_parent_path, UNLOCK) == target_parent);
    } else if (cmp == 1) {
      assert(get_subfolder(tree, source_parent_path, UNLOCK) == source_parent);
    }
  }
exit2:
  if (mode == LOCK) {
    assert (cmp == 1 || cmp == -1);
    if (cmp == -1) {
      assert(get_subfolder(tree, source_parent_path, UNLOCK) == source_parent);
    } else if (cmp == 1) {
      assert(get_subfolder(tree, target_parent_path, UNLOCK) == target_parent);
    }
  } else {
    rwlock_wrunlock(lca->rwlock);
  }

  // if (mode == LOCK) {
  //   get_two_subfolders(tree, source_parent_path, target_parent_path, UNLOCK);
  // }

exit1:
  assert(get_lca(tree, source_parent_path, target_parent_path, UNLOCK) == lca);
exit0:
  free(source_parent_path); 
  free(target_parent_path);

  return result;
}

typedef enum {
  Write,
  Weak
} VisitMode;

void get_two_subfolders(
  Tree *tree, 
  const char *source, 
  const char *target, 
  TraverseMode mode,
  Tree **source_node,
  Tree **target_node,
  rwlock_t *mutexes[],
  int *n_mutexes,
  rwlock_t *end_mutexes[],
  int *n_end_mutexes,
  VisitMode visit_mode
  ) {  

  if (mode == LOCK) { assert(visit_mode == Write); }
  else if (mode == WEAK) { assert(visit_mode == Weak); }

  if (mode == UNLOCK) {
    assert (*n_mutexes >= 2);

    assert(*n_end_mutexes >= 0 && *n_end_mutexes <= 2);
    for (int i=*n_end_mutexes - 1; i >= 0; --i) {
      if (visit_mode == Write) {
        rwlock_wrunlock(end_mutexes[i]);
      }
    }

    for (int i=*n_mutexes - 1; i >=0; --i) {
      rwlock_rdunlock(mutexes[i]);
    }
    return;
  }
  assert(is_path_valid(source) && is_path_valid(target));

  Tree *subtreeA = tree, *subtreeB = tree;
  *source_node = NULL;
  *target_node = NULL;

  char componentA[MAX_FOLDER_NAME_LENGTH + 1];
  char componentB[MAX_FOLDER_NAME_LENGTH + 1];
  const char *subpathA = source, *subpathB = target;

  bool lockedEndA=false, lockedEndB=false;
  *n_mutexes = 0;
  *n_end_mutexes = 0;
  while (subtreeA || subtreeB) {
    if (subpathA) subpathA = split_path(subpathA, componentA);
    if (subpathB) subpathB = split_path(subpathB, componentB);

    if (strcmp(componentA, componentB) <= 0) {

      if (subtreeA && !subpathA && !lockedEndA) {
        lockedEndA = true;
        end_mutexes[(*n_end_mutexes)++] = subtreeA->rwlock;
        if (visit_mode == Write) {
          rwlock_wrlock(subtreeA->rwlock);
        }
      }
      
      if (subtreeB && !subpathB && !lockedEndB) {
        lockedEndB = true;
        end_mutexes[(*n_end_mutexes)++] = subtreeB->rwlock;
        if (visit_mode == Write) {
          rwlock_wrlock(subtreeB->rwlock);
        }
      }
    } else {
      if (subtreeB && !subpathB && !lockedEndB) {
        lockedEndB = true;
        end_mutexes[(*n_end_mutexes)++] = subtreeB->rwlock;
        if (visit_mode == Write) {
          rwlock_wrlock(subtreeB->rwlock);
        }
      }

      if (subtreeA && !subpathA && !lockedEndA) {
        lockedEndA = true;
        end_mutexes[(*n_end_mutexes)++] = subtreeA->rwlock;
        if (visit_mode == Write) {
          rwlock_wrlock(subtreeA->rwlock);
        }
      }
      
    }


    rwlock_t *lockA=NULL, *lockB=NULL;
    if (subpathA && subtreeA) { lockA = subtreeA->rwlock; }
    if (subpathB && subtreeB) { lockB = subtreeB->rwlock; }

    if (mode == LOCK) { 
      if (strcmp(componentA, componentB) <= 0) {
      if (lockA) { mutexes[(*n_mutexes)++] = lockA; rwlock_rdlock(lockA); }
      if (lockB) { mutexes[(*n_mutexes)++] = lockB; rwlock_rdlock(lockB); }
      } else {
      if (lockB) { mutexes[(*n_mutexes)++] = lockB; rwlock_rdlock(lockB); }
        if (lockA) { mutexes[(*n_mutexes)++] = lockA; rwlock_rdlock(lockA); }
      }
    }

    if (subpathA && subtreeA) { subtreeA = (Tree *)hmap_get(subtreeA->hmap, componentA); }
    if (subpathB && subtreeB) { subtreeB = (Tree *)hmap_get(subtreeB->hmap, componentB); }
    if (!subpathA && !subpathB) { break; }
  }

  *source_node = subtreeA;
  *target_node = subtreeB;
}


void breathe(Tree* tree) {
  return;
  const char *key;
  void *value;
  HashMapIterator it = hmap_iterator(tree->hmap);
  while (hmap_next(tree->hmap, &it, &key, &value)) {
    Tree *child = (Tree *)value;
    breathe(child);
  }

  rwlock_rdlock(tree->rwlock);
  rwlock_rdunlock(tree->rwlock);
  rwlock_wrlock(tree->rwlock);
  rwlock_wrunlock(tree->rwlock);

  return;
}

// to jest juz wersja ostateczna, ktora robi calkowitego BFSa z porzadkowaniem
// na poszczegolnych poziomach
// niestety nie dziala xddd i nie bedzie dzialac
// to zadanie mnie przeroslo, te funkcje sa zdecydowanie zbyt skomplikowane
// i zbyt error prone
int tree_moveFAST(Tree* tree, const char* source, const char* target) {
  breathe(tree);
  if (!source || !is_path_valid(source)) { return EINVAL; }
  if (!target || !is_path_valid(target)) { return EINVAL; }
  if (!strcmp(source, "/")) { return EBUSY; }
  if (!strcmp(target, "/")) { return EEXIST; }
  
  char source_component[MAX_FOLDER_NAME_LENGTH + 1];
  char *source_parent_path = make_path_to_parent(source, source_component);

  char target_component[MAX_FOLDER_NAME_LENGTH + 1];
  char *target_parent_path = make_path_to_parent(target, target_component);

  int result = 0;
  if (starts_with(target, source)) { result = EINVMV; goto exit0; }
  if (starts_with(source, target)) {
    Tree *node = get_subfolder(tree, source, LOCK);
    assert(get_subfolder(tree, source, UNLOCK) == node);
    result = node ? EEXIST : ENOENT;
    goto exit0;
  }
  Tree *lca = NULL;
  bool release_lca = false;
  

  int cmp = strcmp(source_parent_path, target_parent_path);
  cmp = cmp ? cmp / abs(cmp) : 0;
  TraverseMode mode;

  if (!cmp || starts_with(source_parent_path, target_parent_path) || starts_with(target_parent_path, source_parent_path) ) {
    lca = get_lca(tree, source_parent_path, target_parent_path, LOCK);
    release_lca=true;
    if (!lca) { result = ENOENT; goto exit1; }
    rwlock_wrlock(lca->rwlock);
    mode = WEAK;
  } else {
    mode = LOCK;
  }

  Tree *source_parent, *target_parent;
  rwlock_t *mutexes[MAX_PATH_LENGTH * 2], *end_mutexes[2];
  int n_mutexes, n_end_mutexes;

  VisitMode visit_mode = mode == LOCK ? Write : Weak;
  get_two_subfolders(
    tree, 
    source_parent_path, 
    target_parent_path, 
    mode, 
    &source_parent, 
    &target_parent, 
    mutexes, 
    &n_mutexes, 
    end_mutexes, 
    &n_end_mutexes, 
    visit_mode
  );
  if (!source_parent || !target_parent) { result = ENOENT; goto exit2; }
  
  Tree *source_node = hmap_get(source_parent->hmap, source_component);
  if (!source_node) { result = ENOENT; goto exit2; }
  
  assert(hmap_remove(source_parent->hmap, source_component));
  bool success = hmap_insert(target_parent->hmap, target_component, source_node);
  if (!success) {
    assert(hmap_insert(source_parent->hmap, source_component, source_node));
    result = EEXIST;
  }

exit2:
  if (mode == LOCK) {
    get_two_subfolders(tree, NULL, NULL, UNLOCK, NULL, NULL, mutexes, &n_mutexes, end_mutexes, &n_end_mutexes, Write);
  } else if (mode == WEAK) {
    rwlock_wrunlock(lca->rwlock);
  }

exit1:
  if (release_lca) {
    assert(get_lca(tree, source_parent_path, target_parent_path, UNLOCK) == lca);
  }
exit0:
  free(source_parent_path); 
  free(target_parent_path);

  breathe(tree);
  return result;
}

