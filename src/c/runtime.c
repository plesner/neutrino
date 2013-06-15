#include "alloc.h"
#include "runtime.h"
#include "value-inl.h"

value_t roots_init(roots_t *roots, runtime_t *runtime) {
  // First set up the meta-species which it its own species.
  TRY_DEF(meta, new_heap_species(runtime, otSpecies));
  set_object_species(meta, meta);
  roots->species_species = meta;

  // The other species are straightforward.
  TRY_SET(roots->string_species, new_heap_species(runtime, otString));
  TRY_SET(roots->array_species, new_heap_species(runtime, otArray));
  TRY_SET(roots->null_species, new_heap_species(runtime, otNull));

  // Singletons
  TRY_SET(roots->null, new_heap_null(runtime));
  return non_signal();
}

void roots_clear(roots_t *roots) {
  roots->species_species = non_signal();
  roots->string_species = non_signal();
  roots->array_species = non_signal();
  roots->null_species = non_signal();
  roots->null = non_signal();
}

value_t runtime_init(runtime_t *runtime, space_config_t *config) {
  // First reset all the fields to a well-defined value.
  runtime_clear(runtime);
  TRY(heap_init(&runtime->heap, config));
  TRY(roots_init(&runtime->roots, runtime));
  return non_signal();
}

void runtime_clear(runtime_t *runtime) {
  roots_clear(&runtime->roots);
}

void runtime_dispose(runtime_t *runtime) {
  heap_dispose(&runtime->heap);
}

value_t runtime_null(runtime_t *runtime) {
  return runtime->roots.null;
}
