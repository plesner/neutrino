// Copyright 2013 the Neutrino authors (see AUTHORS).
// Licensed under the Apache License, Version 2.0 (see LICENSE).

#include "alloc.h"
#include "behavior.h"
#include "bind.h"
#include "file.h"
#include "interp.h"
#include "log.h"
#include "runtime.h"
#include "tagged.h"

// --- B i n d i n g ---

void binding_context_init(binding_context_t *context, value_t ambience) {
  context->bound_module_map = whatever();
  context->fragment_entry_map = whatever();
  context->ambience = ambience;
}

// Returns the unbound fragment for the given fragment.
static value_t get_fragment_entry_fragment(value_t entry) {
  return get_tuple_at(entry, 0);
}

// Returns the imports array buffer for the given fragment.
static value_t get_fragment_entry_imports(value_t entry) {
  return get_tuple_at(entry, 1);
}

// Returns the name of the fragment described by the given entry.
static value_t get_fragment_entry_identifier(value_t entry) {
  return get_tuple_at(entry, 2);
}

// Checks whether a fragment entry for the given stage and path already exists
// and if not creates it.
static value_t binding_context_ensure_fragment_entry(binding_context_t *context,
    value_t stage, value_t path, value_t fragment, bool *created) {
  CHECK_PHYLUM(tpStageOffset, stage);
  CHECK_FAMILY(ofPath, path);
  CHECK_FAMILY_OPT(ofUnboundModuleFragment, fragment);
  value_t path_map = context->fragment_entry_map;
  runtime_t *runtime = get_ambience_runtime(context->ambience);
  if (!has_id_hash_map_at(path_map, path)) {
    TRY_DEF(stage_map, new_heap_id_hash_map(runtime, 16));
    TRY(set_id_hash_map_at(runtime, path_map, path, stage_map));
  }
  value_t stage_map = get_id_hash_map_at(path_map, path);
  if (!has_id_hash_map_at(stage_map, stage)) {
    TRY_DEF(imports, new_heap_array_buffer(runtime, 4));
    TRY_DEF(ident, new_heap_identifier(runtime, stage, path));
    TRY_DEF(entry, new_heap_triple(runtime, fragment, imports, ident));
    TRY(set_id_hash_map_at(runtime, stage_map, stage, entry));
    *created = true;
  }
  return get_id_hash_map_at(stage_map, stage);
}

static value_t run_expression_until_condition(value_t ambience, value_t fragment,
    value_t expr) {
  runtime_t *runtime = get_ambience_runtime(ambience);
  TRY_DEF(code_block, compile_expression(runtime, expr, fragment,
      scope_get_bottom()));
  return run_code_block_until_condition(ambience, code_block);
}

// Adds a namespace binding based on the given declaration ast in the given
// fragment's namespace.
static value_t apply_namespace_declaration(value_t ambience, value_t decl,
    value_t fragment) {
  CHECK_FAMILY(ofAmbience, ambience);
  CHECK_FAMILY(ofNamespaceDeclarationAst, decl);
  CHECK_FAMILY(ofModuleFragment, fragment);
  runtime_t *runtime = get_ambience_runtime(ambience);
  value_t value_syntax = get_namespace_declaration_ast_value(decl);
  TRY_DEF(code_block, compile_expression(runtime, value_syntax, fragment,
      scope_get_bottom()));
  TRY_DEF(value, run_code_block_until_condition(ambience, code_block));
  value_t nspace = get_module_fragment_namespace(fragment);
  value_t path = get_namespace_declaration_ast_path(decl);
  TRY(set_namespace_binding_at(runtime, nspace, path, value));
  return success();
}

// Validates that it is safe to bind the given implementation to the given
// surface-level method.
static value_t validate_builtin_method_binding(value_t method, value_t impl) {
  value_t signature = get_method_signature(method);
  size_t posc = get_signature_parameter_count(signature)
      - 1  // subject
      - 1; // selector
  size_t required_posc = get_builtin_implementation_argument_count(impl);
  if (posc != required_posc) {
    ERROR("Argument count mismatch (found %i, expected %i) binding %9v to %9v",
        posc, required_posc, impl, signature);
    return new_condition(ccBuiltinBindingFailed);
  }
  return success();
}

// Executes a method declaration on the given fragment.
static value_t apply_method_declaration(value_t ambience, value_t decl,
    value_t fragment) {
  CHECK_FAMILY(ofMethodDeclarationAst, decl);
  CHECK_FAMILY(ofModuleFragment, fragment);
  runtime_t *runtime = get_ambience_runtime(ambience);
  // Look for the :builtin annotation on this method.
  value_t annots = get_method_declaration_ast_annotations(decl);
  value_t builtin_name = new_not_found_condition();
  for (size_t i = 0; i < get_array_length(annots); i++) {
    value_t annot = get_array_at(annots, i);
    TRY_DEF(value, run_expression_until_condition(ambience, fragment, annot));
    if (in_family(ofBuiltinMarker, value))
      builtin_name = get_builtin_marker_name(value);
  }
  // Compile the method whether it's a builtin or not. This way we can reuse
  // the compilation code for both cases and just patch up the result after
  // the fact if it's a builtin.
  value_t method_ast = get_method_declaration_ast_method(decl);
  TRY_DEF(method, compile_method_ast_to_method(runtime, method_ast, fragment));
  if (!in_condition_cause(ccNotFound, builtin_name)) {
    // This is a builtin so patch the method with the builtin implementation.
    TRY_DEF(impl, runtime_get_builtin_implementation(runtime, builtin_name));
    value_t impl_code = get_builtin_implementation_code(impl);
    TRY(validate_builtin_method_binding(method, impl));
    set_method_code(method, impl_code);
    value_t impl_flags = get_builtin_implementation_method_flags(impl);
    set_method_flags(method, impl_flags);
  }
  value_t methodspace = get_module_fragment_methodspace(fragment);
  TRY(add_methodspace_method(runtime, methodspace, method));
  return success();
}

// Executes an is declaration on the given fragment.
static value_t apply_is_declaration(runtime_t *runtime, value_t decl,
    value_t fragment) {
  CHECK_FAMILY(ofIsDeclarationAst, decl);
  CHECK_FAMILY(ofModuleFragment, fragment);
  value_t subtype_ast = get_is_declaration_ast_subtype(decl);
  value_t supertype_ast = get_is_declaration_ast_supertype(decl);
  TRY_DEF(subtype, quick_and_dirty_evaluate_syntax(runtime, fragment, subtype_ast));
  TRY_DEF(supertype, quick_and_dirty_evaluate_syntax(runtime, fragment, supertype_ast));
  value_t methodspace = get_module_fragment_methodspace(fragment);
  TRY(add_methodspace_inheritance(runtime, methodspace, subtype, supertype));
  return success();
}

// Performs the appropriate action for a fragment element to the given fragment.
static value_t apply_unbound_fragment_element(value_t ambience, value_t element,
    value_t fragment) {
  CHECK_FAMILY(ofAmbience, ambience);
  heap_object_family_t family = get_heap_object_family(element);
  switch (family) {
    case ofNamespaceDeclarationAst:
      return apply_namespace_declaration(ambience, element, fragment);
    case ofMethodDeclarationAst:
      return apply_method_declaration(ambience, element, fragment);
    case ofIsDeclarationAst:
      return apply_is_declaration(get_ambience_runtime(ambience), element, fragment);
    default:
      ERROR("Invalid toplevel element %s", get_heap_object_family_name(family));
      return success();
  }
}

// Adds mappings in the namespace and imports in the methodspace for everything
// imported by the given fragment.
static value_t bind_module_fragment_imports(binding_context_t *context,
    value_t imports, value_t bound_fragment) {
  // Import the modules spaces into this fragment and create bindings in the
  // importspace.
  value_t methodspace = get_module_fragment_methodspace(bound_fragment);
  value_t importspace = get_module_fragment_imports(bound_fragment);
  runtime_t *runtime = get_ambience_runtime(context->ambience);
  for (size_t i = 0; i < get_array_buffer_length(imports); i++) {
    // Look up the imported module.
    value_t import_ident = get_array_buffer_at(imports, i);
    value_t import_path = get_identifier_path(import_ident);
    value_t import_head = get_path_head(import_path);
    value_t import_stage = get_identifier_stage(import_ident);
    value_t import_module = get_id_hash_map_at(context->bound_module_map, import_path);
    value_t import_fragment = get_module_fragment_at(import_module, import_stage);
    CHECK_TRUE("import not bound", is_module_fragment_bound(import_fragment));
    value_t import_methods = get_module_fragment_methodspace(import_fragment);
    TRY(add_methodspace_import(runtime, methodspace, import_methods));
    TRY(set_id_hash_map_at(runtime, importspace, import_head,
        import_fragment));
  }
  return success();
}

// Iteratively apply the elements of the unbound fragment to the partially
// initialized bound fragment.
static value_t apply_module_fragment_elements(binding_context_t *context,
    value_t unbound_fragment, value_t bound_fragment) {
  value_t elements = get_unbound_module_fragment_elements(unbound_fragment);
  for (size_t i = 0; i < get_array_length(elements); i++) {
    value_t element = get_array_at(elements, i);
    TRY(apply_unbound_fragment_element(context->ambience, element, bound_fragment));
  }
  return success();
}

// Binds an individual module fragment.
static value_t bind_module_fragment(binding_context_t *context,
    value_t entry, value_t bound_fragment) {
  CHECK_FAMILY(ofModuleFragment, bound_fragment);
  value_t unbound_fragment = get_fragment_entry_fragment(entry);
  value_t imports = get_fragment_entry_imports(entry);
  if (!is_nothing(unbound_fragment)) {
    // This is a real fragment so we have to apply the entries.
    CHECK_FAMILY(ofUnboundModuleFragment, unbound_fragment);
    CHECK_EQ("fragment already bound", feUnbound,
        get_module_fragment_epoch(bound_fragment));
    set_module_fragment_epoch(bound_fragment, feBinding);
    TRY(bind_module_fragment_imports(context, imports, bound_fragment));
    TRY(apply_module_fragment_elements(context, unbound_fragment, bound_fragment));
  }
  set_module_fragment_epoch(bound_fragment, feComplete);
  return success();
}

// Ensures that the given unbound module is in the given array buffer, as well
// as any other modules imported by the module.
static value_t ensure_module_in_array(runtime_t *runtime, value_t array,
    value_t unbound_module) {
  CHECK_FAMILY(ofUnboundModule, unbound_module);
  if (in_array_buffer(array, unbound_module))
    // If it's already there there's nothing to do.
    return success();
  // Add the module.
  TRY(add_to_array_buffer(runtime, array, unbound_module));
  // Scan through the imports and recursively add imported modules. Which
  // stage the module is imported into doesn't matter at this point, we just
  // have to enumerate them.
  value_t unbound_fragments = get_unbound_module_fragments(unbound_module);
  for (size_t fi = 0; fi < get_array_length(unbound_fragments); fi++) {
    value_t unbound_fragment = get_array_at(unbound_fragments, fi);
    value_t imports = get_unbound_module_fragment_imports(unbound_fragment);
    for (size_t ii = 0; ii < get_array_length(imports); ii++) {
      value_t import = get_array_at(imports, ii);
      TRY_DEF(imported_module, module_loader_lookup_module(
          deref(runtime->module_loader), import));
      TRY(ensure_module_in_array(runtime, array, imported_module));
    }
  }
  return success();
}

// Builds an array buffer containing all the modules that are needed to load
// the given unbound module (which is itself added to the array too).
static value_t build_transitive_module_array(runtime_t *runtime,
    value_t unbound_module) {
  CHECK_FAMILY(ofUnboundModule, unbound_module);
  TRY_DEF(result, new_heap_array_buffer(runtime, 16));
  TRY(ensure_module_in_array(runtime, result, unbound_module));
  return result;
}

static value_t init_empty_module_fragment(runtime_t *runtime, value_t fragment) {
  TRY_DEF(nspace, new_heap_namespace(runtime, nothing()));
  TRY_DEF(methodspace, new_heap_methodspace(runtime));
  TRY_DEF(imports, new_heap_id_hash_map(runtime, 16));
  set_module_fragment_namespace(fragment, nspace);
  set_module_fragment_methodspace(fragment, methodspace);
  set_module_fragment_imports(fragment, imports);
  return success();
}

// Creates a new empty but suitably initialized bound module fragment.
static value_t new_empty_module_fragment(runtime_t *runtime, value_t stage,
    value_t module) {
  TRY_DEF(empty_fragment, new_heap_module_fragment(runtime, module, stage,
      nothing(), nothing(), nothing()));
  TRY(init_empty_module_fragment(runtime, empty_fragment));
  return empty_fragment;
}

// Returns true iff the given identifier is $:core.
static bool is_present_core(runtime_t *runtime, value_t ident) {
  CHECK_FAMILY(ofIdentifier, ident);
  value_t path = get_identifier_path(ident);
  value_t stage = get_identifier_stage(ident);
  if (get_stage_offset_value(stage) != 0)
    // Not present.
    return false;
  if (is_path_empty(path) || !is_path_empty(get_path_tail(path)))
    // Not length 1.
    return false;
  value_t head = get_path_head(path);
  return value_identity_compare(head, RSTR(runtime, core));
}

// Creates and binds modules and fragments according to the given schedule.
static value_t execute_binding_schedule(binding_context_t *context, value_t schedule) {
  runtime_t *runtime = get_ambience_runtime(context->ambience);
  for (size_t i = 0; i < get_array_buffer_length(schedule); i++) {
    value_t next = get_array_buffer_at(schedule, i);
    TOPIC_INFO(Library, "About to bind %v", next);
    value_t path = get_identifier_path(next);
    value_t stage = get_identifier_stage(next);
    // Create the bound module if it doesn't already exist.
    value_t bound_module = get_id_hash_map_at(context->bound_module_map, path);
    if (in_condition_cause(ccNotFound, bound_module)) {
      TRY_SET(bound_module, new_heap_empty_module(runtime, path));
      TRY(set_id_hash_map_at(runtime, context->bound_module_map, path,
          bound_module));
    }
    // Create the bound fragment.
    value_t bound_fragment = get_module_fragment_at(bound_module, stage);
    if (in_condition_cause(ccNotFound, bound_fragment)) {
      TRY_SET(bound_fragment, new_empty_module_fragment(runtime, stage,
          bound_module));
      TRY(add_module_fragment(runtime, bound_module, bound_fragment));
    } else {
      // An earlier phase needed a reference to this fragment so it has already
      // been created but not initialized yet.
      CHECK_EQ("Unexpected phase", get_module_fragment_epoch(bound_fragment),
          feUninitialized);
      TRY(init_empty_module_fragment(runtime, bound_fragment));
      set_module_fragment_epoch(bound_fragment, feUnbound);
    }
    if (is_present_core(runtime, next)) {
      // TODO: this is a hack, there should be some other mechanism for
      //   identifying the core module. This way of binding it also means it
      //   gets bound at a time that's not particularly well-defined which is
      //   also an issue.
      TOPIC_INFO(Library, "Binding present core to %v", bound_fragment);
      set_ambience_present_core_fragment(context->ambience, bound_fragment);
    }
    // Grab the unbound fragment we'll use to create the bound fragment.
    value_t module_entries = get_id_hash_map_at(context->fragment_entry_map, path);
    value_t fragment_entry = get_id_hash_map_at(module_entries, stage);
    // Bind the fragment based on the data from the entry.
    TRY(bind_module_fragment(context, fragment_entry, bound_fragment));
    TOPIC_INFO(Library, "Done binding %v", next);
  }
  return success();
}

value_t build_bound_module(value_t ambience, value_t unbound_module) {
  runtime_t *runtime = get_ambience_runtime(ambience);
  binding_context_t context;
  binding_context_init(&context, ambience);
  TRY_SET(context.bound_module_map, new_heap_id_hash_map(runtime, 16));
  TRY_DEF(modules, build_transitive_module_array(runtime, unbound_module));
  TRY(build_fragment_entry_map(&context, modules));
  TRY_DEF(schedule, build_binding_schedule(&context));
  TRY(execute_binding_schedule(&context, schedule));
  value_t path = get_unbound_module_path(unbound_module);
  value_t result = get_id_hash_map_at(context.bound_module_map, path);
  CHECK_FALSE("module missing", in_condition_cause(ccNotFound, result));
  return result;
}

// Given an array of modules map builds a two-level map from paths to stages to
// fragment entries.
static value_t build_real_fragment_entries(binding_context_t *context,
    value_t modules) {
  runtime_t *runtime = get_ambience_runtime(context->ambience);
  for (size_t mi = 0; mi < get_array_buffer_length(modules); mi++) {
    value_t module = get_array_buffer_at(modules, mi);
    value_t path = get_unbound_module_path(module);
    value_t fragments = get_unbound_module_fragments(module);
    for (size_t fi = 0; fi < get_array_length(fragments); fi++) {
      value_t fragment = get_array_at(fragments, fi);
      value_t stage = get_unbound_module_fragment_stage(fragment);
      bool dummy = false;
      TRY_DEF(entry, binding_context_ensure_fragment_entry(context, stage,
          path, fragment, &dummy));
      value_t imports = get_fragment_entry_imports(entry);
      value_t fragment_imports = get_unbound_module_fragment_imports(fragment);
      for (size_t ii = 0; ii < get_array_length(fragment_imports); ii++) {
        value_t import = get_array_at(fragment_imports, ii);
        TRY_DEF(ident, new_heap_identifier(runtime, present_stage(), import));
        TRY(ensure_array_buffer_contains(runtime, imports, ident));
      }
    }
  }
  return success();
}

// Add synthetic fragment entries corresponding to imported fragments where
// there is no real fragment to import the fragment into.
static value_t build_synthetic_fragment_entries(binding_context_t *context) {
  // Keep adding synthetic modules as long as changes are being made to the
  // map. We'll scan through the fragments currently in the map, then scan
  // through their imports, and for each check that the fragment that should
  // receive the import exists. If it doesn't it is created.
  loop: do {
    id_hash_map_iter_t module_iter;
    id_hash_map_iter_init(&module_iter, context->fragment_entry_map);
    while (id_hash_map_iter_advance(&module_iter)) {
      value_t module_path;
      value_t module_fragments;
      // Scan through the fragments.
      id_hash_map_iter_get_current(&module_iter, &module_path, &module_fragments);
      id_hash_map_iter_t fragment_iter;
      id_hash_map_iter_init(&fragment_iter, module_fragments);
      while (id_hash_map_iter_advance(&fragment_iter)) {
        value_t stage;
        value_t entry;
        id_hash_map_iter_get_current(&fragment_iter, &stage, &entry);
        value_t unbound_fragment = get_fragment_entry_fragment(entry);
        // If there is no fragment associated with this entry it is synthetic
        // and hence we're done.
        if (is_nothing(unbound_fragment))
          continue;
        // Scan through the fragment's imports and ensure that their import
        // targets have been created.
        value_t imports = get_fragment_entry_imports(entry);
        for (size_t i = 0; i < get_array_buffer_length(imports); i++) {
          value_t import = get_array_buffer_at(imports, i);
          value_t import_fragment_stage = get_identifier_stage(import);
          if (!value_identity_compare(import_fragment_stage, present_stage()))
            // We'll record past imports but ignore them for the purposes of
            // closing the import map since they're redundant.
            continue;
          value_t import_module_path = get_identifier_path(import);
          value_t import_module = get_id_hash_map_at(context->fragment_entry_map,
              import_module_path);
          // Scan through the fragments of the imported module.
          id_hash_map_iter_t imported_fragment_iter;
          id_hash_map_iter_init(&imported_fragment_iter, import_module);
          bool has_changed_anything = false;
          while (id_hash_map_iter_advance(&imported_fragment_iter)) {
            value_t import_stage;
            value_t import_entry;
            id_hash_map_iter_get_current(&imported_fragment_iter,
                &import_stage, &import_entry);
            value_t target_stage = add_stage_offsets(import_stage, stage);
            // Ensure that there is a target entry to add the import to. If it
            // already exists this is a no-op, if it doesn't a synthetic entry
            // is created.
            TRY_DEF(target_entry, binding_context_ensure_fragment_entry(
                context, target_stage, module_path, nothing(),
                &has_changed_anything));
           value_t target_imports = get_fragment_entry_imports(target_entry);
           value_t import_ident = get_fragment_entry_identifier(import_entry);
            if (!in_array_buffer(target_imports, import_ident)) {
              has_changed_anything = true;
              TRY(add_to_array_buffer(get_ambience_runtime(context->ambience),
                  target_imports, import_ident));
            }
          }
          // If any changes were made we have to start over.
          if (has_changed_anything)
            goto loop;
        }
      }
    }
  } while (false);
  return success();
}

value_t build_fragment_entry_map(binding_context_t *context, value_t modules) {
  runtime_t *runtime = get_ambience_runtime(context->ambience);
  TRY_SET(context->fragment_entry_map, new_heap_id_hash_map(runtime, 16));
  TRY(build_real_fragment_entries(context, modules));
  TRY(build_synthetic_fragment_entries(context));
  return context->fragment_entry_map;
}

// Returns true if the given path and stage have already been scheduled to be
// bound in the given schedule.
static bool is_fragment_scheduled(value_t schedule, value_t ident) {
  return in_array_buffer(schedule, ident);
}

// Uses the fragment entry map to create an array of identifiers for all the
// fragments, synthetic and real.
static value_t build_fragment_identifier_array(binding_context_t *context) {
  runtime_t *runtime = get_ambience_runtime(context->ambience);
  TRY_DEF(result, new_heap_array_buffer(runtime, 16));
  id_hash_map_iter_t module_iter;
  id_hash_map_iter_init(&module_iter, context->fragment_entry_map);
  while (id_hash_map_iter_advance(&module_iter)) {
    value_t module_path;
    value_t module_fragments;
    // Scan through the fragments.
    id_hash_map_iter_get_current(&module_iter, &module_path, &module_fragments);
    id_hash_map_iter_t fragment_iter;
    id_hash_map_iter_init(&fragment_iter, module_fragments);
    while (id_hash_map_iter_advance(&fragment_iter)) {
      value_t stage;
      value_t entry;
      id_hash_map_iter_get_current(&fragment_iter, &stage, &entry);
      value_t ident = get_fragment_entry_identifier(entry);
      TRY(add_to_array_buffer(runtime, result, ident));
    }
  }
  sort_array_buffer(result);
  return result;
}

// Returns the entry corresponding to the fragment immediately preceding the
// fragment with the given stage in the given module. If there is no such entry
// NotFound is returned.
static value_t get_fragment_entry_before(value_t module, value_t stage) {
  // Simply scan through the entries one at a time, keeping track of the closest
  // one before the given stage.
  int32_t stage_offset = get_stage_offset_value(stage);
  int32_t closest_stage_value = kMostNegativeInt32;
  value_t closest_stage_entry = new_not_found_condition();
  id_hash_map_iter_t fragment_iter;
  id_hash_map_iter_init(&fragment_iter, module);
  while (id_hash_map_iter_advance(&fragment_iter)) {
    value_t fragment_stage;
    value_t fragment_entry;
    id_hash_map_iter_get_current(&fragment_iter, &fragment_stage, &fragment_entry);
    int32_t fragment_stage_offset = get_stage_offset_value(fragment_stage);
    if (fragment_stage_offset >= stage_offset)
      // This stage isn't before the one we're looking for.
      continue;
    if (fragment_stage_offset > closest_stage_value) {
      // This one is better than the best we've seen so far.
      closest_stage_value = fragment_stage_offset;
      closest_stage_entry = fragment_entry;
    }
  }
  return closest_stage_entry;
}

// Returns true iff the given identifier corresponds to a fragment that is
// ready to be bound and hasn't already been bound.
static bool should_fragment_be_bound(binding_context_t *context, value_t schedule,
    value_t ident) {
  // This fragment is already scheduled so we definitely don't want to bind it
  // again.
  if (is_fragment_scheduled(schedule, ident))
    return false;
  // Grab the information we hold about the fragment.
  value_t path = get_identifier_path(ident);
  value_t stage = get_identifier_stage(ident);
  value_t module = get_id_hash_map_at(context->fragment_entry_map, path);
  value_t entry = get_id_hash_map_at(module, stage);
  value_t imports = get_fragment_entry_imports(entry);
  // Check whether all its explicit dependencies are satisfied.
  for (size_t i = 0; i < get_array_buffer_length(imports); i++) {
    value_t import = get_array_buffer_at(imports, i);
    if (!is_fragment_scheduled(schedule, import))
      return false;
  }
  // Check if there is a preceding fragment and whether it has been bound.
  value_t entry_before = get_fragment_entry_before(module, stage);
  if (in_condition_cause(ccNotFound, entry_before)) {
    return true;
  } else {
    value_t before_ident = get_fragment_entry_identifier(entry_before);
    return is_fragment_scheduled(schedule, before_ident);
  }
}

value_t build_binding_schedule(binding_context_t *context) {
  runtime_t *runtime = get_ambience_runtime(context->ambience);
  TRY_DEF(schedule, new_heap_array_buffer(runtime, 16));
  TRY_DEF(all_fragments, build_fragment_identifier_array(context));
  loop: do {
    for (size_t i = 0; i < get_array_buffer_length(all_fragments); i++) {
      value_t ident = get_array_buffer_at(all_fragments, i);
      if (should_fragment_be_bound(context, schedule, ident)) {
        TRY(add_to_array_buffer(runtime, schedule, ident));
        goto loop;
      }
    }
  } while (false);
  return schedule;
}


// --- M o d u l e   l o a d e r ---

FIXED_GET_MODE_IMPL(module_loader, vmMutable);

ACCESSORS_IMPL(ModuleLoader, module_loader, acInFamilyOpt, ofIdHashMap, Modules, modules);

value_t module_loader_validate(value_t self) {
  VALIDATE_FAMILY(ofModuleLoader, self);
  return success();
}

// Reads a library from the given library path and adds the modules to this
// loaders set of available modules.
static value_t module_loader_read_library(runtime_t *runtime, value_t self,
    value_t library_path) {
  // Read the library from the file.
  string_t library_path_str;
  get_string_contents(library_path, &library_path_str);
  TRY_DEF(data, read_file_to_blob(runtime, &library_path_str));
  TRY_DEF(library, runtime_plankton_deserialize(runtime, data));
  if (!in_family(ofLibrary, library))
    return new_invalid_input_condition();
  set_library_display_name(library, library_path);
  // Load all the modules from the library into this module loader.
  id_hash_map_iter_t iter;
  id_hash_map_iter_init(&iter, get_library_modules(library));
  while (id_hash_map_iter_advance(&iter)) {
    value_t key;
    value_t value;
    id_hash_map_iter_get_current(&iter, &key, &value);
    TRY(set_id_hash_map_at(runtime, get_module_loader_modules(self), key, value));
  }
  return success();
}

value_t module_loader_process_options(runtime_t *runtime, value_t self,
    value_t options) {
  CHECK_FAMILY(ofIdHashMap, options);
  value_t libraries = get_id_hash_map_at_with_default(options, RSTR(runtime, libraries),
      ROOT(runtime, empty_array));
  for (size_t i = 0; i < get_array_length(libraries); i++) {
    value_t library_path = get_array_at(libraries, i);
    TRY(module_loader_read_library(runtime, self, library_path));
  }
  return success();
}

void module_loader_print_on(value_t value, print_on_context_t *context) {
  string_buffer_printf(context->buf, "#<module loader ");
  value_t modules = get_module_loader_modules(value);
  value_print_inner_on(modules, context, -1);
  string_buffer_printf(context->buf, ">");
}

value_t module_loader_lookup_module(value_t self, value_t path) {
  value_t modules = get_module_loader_modules(self);
  value_t result = get_id_hash_map_at(modules, path);
  if (in_condition_cause(ccNotFound, result))
    WARN("Module %v not found.", path);
  return result;
}


// --- L i b r a r y ---

FIXED_GET_MODE_IMPL(library, vmMutable);

ACCESSORS_IMPL(Library, library, acNoCheck, 0, DisplayName, display_name);
ACCESSORS_IMPL(Library, library, acInFamilyOpt, ofIdHashMap, Modules, modules);

value_t library_validate(value_t self) {
  VALIDATE_FAMILY(ofLibrary, self);
  return success();
}

value_t plankton_new_library(runtime_t *runtime) {
  return new_heap_library(runtime, nothing(), nothing());
}

value_t plankton_set_library_contents(value_t object, runtime_t *runtime,
    value_t contents) {
  UNPACK_PLANKTON_MAP(contents, modules);
  set_library_modules(object, modules_value);
  return success();
}

void library_print_on(value_t value, print_on_context_t *context) {
  string_buffer_printf(context->buf, "#<library(");
  value_t display_name = get_library_display_name(value);
  value_print_inner_on(display_name, context, -1);
  string_buffer_printf(context->buf, ") ");
  value_t modules = get_library_modules(value);
  value_print_inner_on(modules, context, -1);
  string_buffer_printf(context->buf, ">");
}


// --- U n b o u n d   m o d u l e ---

FIXED_GET_MODE_IMPL(unbound_module, vmMutable);

ACCESSORS_IMPL(UnboundModule, unbound_module, acInFamilyOpt, ofPath, Path, path);
ACCESSORS_IMPL(UnboundModule, unbound_module, acInFamilyOpt, ofArray, Fragments, fragments);

value_t unbound_module_validate(value_t self) {
  VALIDATE_FAMILY(ofUnboundModule, self);
  return success();
}

value_t plankton_new_unbound_module(runtime_t *runtime) {
  return new_heap_unbound_module(runtime, nothing(),
      nothing());
}

value_t plankton_set_unbound_module_contents(value_t object, runtime_t *runtime,
    value_t contents) {
  UNPACK_PLANKTON_MAP(contents, path, fragments);
  set_unbound_module_path(object, path_value);
  set_unbound_module_fragments(object, fragments_value);
  return success();
}

void unbound_module_print_on(value_t value, print_on_context_t *context) {
  string_buffer_printf(context->buf, "#<unbound_module(");
  value_t path = get_unbound_module_path(value);
  value_print_inner_on(path, context, -1);
  string_buffer_printf(context->buf, ") ");
  value_t fragments = get_unbound_module_fragments(value);
  value_print_inner_on(fragments, context, -1);
  string_buffer_printf(context->buf, ">");
}


// --- U n b o u n d   m o d u l e   f r a g m e n t ---

FIXED_GET_MODE_IMPL(unbound_module_fragment, vmMutable);

ACCESSORS_IMPL(UnboundModuleFragment, unbound_module_fragment, acNoCheck, 0,
    Stage, stage);
ACCESSORS_IMPL(UnboundModuleFragment, unbound_module_fragment, acInFamilyOpt,
    ofArray, Imports, imports);
ACCESSORS_IMPL(UnboundModuleFragment, unbound_module_fragment, acInFamilyOpt,
    ofArray, Elements, elements);

value_t unbound_module_fragment_validate(value_t self) {
  VALIDATE_FAMILY(ofUnboundModuleFragment, self);
  return success();
}

value_t plankton_new_unbound_module_fragment(runtime_t *runtime) {
  return new_heap_unbound_module_fragment(runtime, nothing(),
      nothing(), nothing());
}

value_t plankton_set_unbound_module_fragment_contents(value_t object, runtime_t *runtime,
    value_t contents) {
  UNPACK_PLANKTON_MAP(contents, stage, imports, elements);
  set_unbound_module_fragment_stage(object, new_stage_offset(get_integer_value(stage_value)));
  set_unbound_module_fragment_imports(object, imports_value);
  set_unbound_module_fragment_elements(object, elements_value);
  return success();
}

void unbound_module_fragment_print_on(value_t value, print_on_context_t *context) {
  string_buffer_printf(context->buf, "#<unbound_module_fragment(");
  value_t stage = get_unbound_module_fragment_stage(value);
  value_print_inner_on(stage, context, -1);
  string_buffer_printf(context->buf, ") imports: ");
  value_t imports = get_unbound_module_fragment_imports(value);
  value_print_inner_on(imports, context, -1);
  string_buffer_printf(context->buf, ") elements: ");
  value_t elements = get_unbound_module_fragment_elements(value);
  value_print_inner_on(elements, context, -1);
  string_buffer_printf(context->buf, ">");
}
