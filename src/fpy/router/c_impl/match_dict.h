#pragma once

#include <Python.h>

typedef struct {
  const char* key;
  size_t key_length;
  const char* value;
  size_t value_length;
} MatchDictEntry;


PyObject*
MatchDict_entries_to_dict(MatchDictEntry* entries, size_t length);
