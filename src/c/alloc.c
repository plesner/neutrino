// Copyright 2013 the Neutrino authors (see AUTHORS).
// Licensed under the Apache License, Version 2.0 (see LICENSE).

#include "alloc.h"
#include "behavior.h"
#include "ctrino.h"
#include "derived.h"
#include "process.h"
#include "tagged.h"
#include "try-inl.h"
#include "value-inl.h"


// --- B a s i c ---

// Run a couple of sanity checks before returning the value from a constructor.
// Returns a condition if the check fails, otherwise returns the given value.
static value_t post_create_sanity_check(value_t value, size_t size) {
  TRY(heap_object_validate(value));
  heap_object_layout_t layout;
  heap_object_layout_init(&layout);
  get_heap_object_layout(value, &layout);
  COND_CHECK_EQ("post create sanity", ccValidationFailed, layout.size, size);
  return value;
}

// Post-processes an allocation result appropriately based on the given set of
// allocation flags.
static value_t post_process_result(runtime_t *runtime, value_t result,
    alloc_flags_t flags) {
  if (flags == afFreeze)
    TRY(ensure_frozen(runtime, result));
  return success();
}

value_t new_heap_uninitialized_roots(runtime_t *runtime) {
  size_t size = kRootsSize;
  TRY_DEF(result, alloc_heap_object(runtime, size, whatever()));
  for (size_t i = 0; i < kRootCount; i++)
    *access_heap_object_field(result, HEAP_OBJECT_FIELD_OFFSET(i)) = whatever();
  return result;
}

value_t new_heap_mutable_roots(runtime_t *runtime) {
  TRY_DEF(argument_map_trie_root, new_heap_argument_map_trie(runtime,
      ROOT(runtime, empty_array)));
  size_t size = kMutableRootsSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_mutable_roots_species)));
  RAW_MROOT(result, argument_map_trie_root) = argument_map_trie_root;
  return result;
}

value_t new_heap_string(runtime_t *runtime, string_t *contents) {
  size_t size = calc_string_size(string_length(contents));
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, string_species)));
  set_string_length(result, string_length(contents));
  string_copy_to(contents, get_string_chars(result), string_length(contents) + 1);
  return post_create_sanity_check(result, size);
}

value_t new_heap_blob(runtime_t *runtime, size_t length) {
  size_t size = calc_blob_size(length);
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, blob_species)));
  set_blob_length(result, length);
  blob_t data;
  get_blob_data(result, &data);
  blob_fill(&data, 0);
  return post_create_sanity_check(result, size);
}

value_t new_heap_blob_with_data(runtime_t *runtime, blob_t *contents) {
  // Allocate the blob object to hold the data.
  TRY_DEF(blob, new_heap_blob(runtime, blob_byte_length(contents)));
  // Pull out the contents of the heap blob.
  blob_t blob_data;
  get_blob_data(blob, &blob_data);
  // Copy the contents into the heap blob.
  blob_copy_to(contents, &blob_data);
  return blob;
}

value_t new_heap_instance_species(runtime_t *runtime, value_t primary,
    value_t manager) {
  size_t size = kInstanceSpeciesSize;
  CHECK_FAMILY(ofType, primary);
  CHECK_FAMILY_OPT(ofInstanceManager, manager);
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_species_species)));
  set_species_instance_family(result, ofInstance);
  set_species_family_behavior(result, &kInstanceBehavior);
  set_species_division_behavior(result, &kInstanceSpeciesBehavior);
  set_instance_species_primary_type_field(result, primary);
  set_instance_species_manager(result, manager);
  return post_create_sanity_check(result, size);
}

value_t new_heap_compact_species(runtime_t *runtime, family_behavior_t *behavior) {
  size_t bytes = kCompactSpeciesSize;
  TRY_DEF(result, alloc_heap_object(runtime, bytes,
      ROOT(runtime, mutable_species_species)));
  set_species_instance_family(result, behavior->family);
  set_species_family_behavior(result, behavior);
  set_species_division_behavior(result, &kCompactSpeciesBehavior);
  return post_create_sanity_check(result, bytes);
}

value_t new_heap_modal_species_unchecked(runtime_t *runtime,
    family_behavior_t *behavior, value_mode_t mode, root_key_t base_root) {
  size_t size = kModalSpeciesSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_species_species)));
  set_species_instance_family(result, behavior->family);
  set_species_family_behavior(result, behavior);
  set_species_division_behavior(result, &kModalSpeciesBehavior);
  set_modal_species_mode(result, mode);
  set_modal_species_base_root(result, base_root);
  return result;
}

value_t new_heap_modal_species(runtime_t *runtime, family_behavior_t *behavior,
    value_mode_t mode, root_key_t base_root) {
  TRY_DEF(result, new_heap_modal_species_unchecked(runtime, behavior, mode, base_root));
  return post_create_sanity_check(result, kModalSpeciesSize);
}

value_t new_heap_array(runtime_t *runtime, size_t length) {
  size_t size = calc_array_size(length);
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_array_species)));
  set_array_length(result, length);
  for (size_t i = 0; i < length; i++)
    set_array_at(result, i, null());
  return post_create_sanity_check(result, size);
}

value_t new_heap_reference(runtime_t *runtime, value_t value) {
  size_t size = kReferenceSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_reference_species)));
  set_reference_value(result, value);
  return post_create_sanity_check(result, size);
}

value_t new_heap_pair(runtime_t *runtime, value_t e0, value_t e1) {
  TRY_DEF(result, new_heap_array(runtime, 2));
  set_array_at(result, 0, e0);
  set_array_at(result, 1, e1);
  TRY(ensure_frozen(runtime, result));
  return result;
}

value_t new_heap_triple(runtime_t *runtime, value_t e0, value_t e1,
    value_t e2) {
  TRY_DEF(result, new_heap_array(runtime, 3));
  set_array_at(result, 0, e0);
  set_array_at(result, 1, e1);
  set_array_at(result, 2, e2);
  TRY(ensure_frozen(runtime, result));
  return result;
}

value_t new_heap_pair_array(runtime_t *runtime, size_t length) {
  return new_heap_array(runtime, length << 1);
}

value_t new_heap_array_buffer(runtime_t *runtime, size_t initial_capacity) {
  size_t size = kArrayBufferSize;
  TRY_DEF(elements, new_heap_array(runtime, initial_capacity));
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_array_buffer_species)));
  set_array_buffer_elements(result, elements);
  set_array_buffer_length(result, 0);
  return post_create_sanity_check(result, size);
}

value_t new_heap_array_buffer_with_contents(runtime_t *runtime, value_t elements) {
  CHECK_FAMILY(ofArray, elements);
  size_t size = kArrayBufferSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_array_buffer_species)));
  set_array_buffer_elements(result, elements);
  set_array_buffer_length(result, get_array_length(elements));
  return post_create_sanity_check(result, size);
}

static value_t new_heap_id_hash_map_entry_array(runtime_t *runtime, size_t capacity) {
  return new_heap_array(runtime, capacity * kIdHashMapEntryFieldCount);
}

value_t new_heap_id_hash_map(runtime_t *runtime, size_t init_capacity) {
  CHECK_REL("invalid initial capacity", init_capacity, >, 0);
  TRY_DEF(entries, new_heap_id_hash_map_entry_array(runtime, init_capacity));
  size_t size = kIdHashMapSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_id_hash_map_species)));
  set_id_hash_map_entry_array(result, entries);
  set_id_hash_map_size(result, 0);
  set_id_hash_map_capacity(result, init_capacity);
  set_id_hash_map_occupied_count(result, 0);
  return post_create_sanity_check(result, size);
}

value_t new_heap_ctrino(runtime_t *runtime) {
  size_t size = kCtrinoSize;
  return alloc_heap_object(runtime, size, ROOT(runtime, ctrino_species));
}

value_t new_heap_key(runtime_t *runtime, value_t display_name) {
  size_t size = kKeySize;
  size_t id = runtime->next_key_index++;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_key_species)));
  set_key_id(result, id);
  set_key_display_name(result, display_name);
  return post_create_sanity_check(result, size);
}

value_t new_heap_instance(runtime_t *runtime, value_t species) {
  CHECK_DIVISION(sdInstance, species);
  TRY_DEF(fields, new_heap_id_hash_map(runtime, 16));
  size_t size = kInstanceSize;
  TRY_DEF(result, alloc_heap_object(runtime, size, species));
  set_instance_fields(result, fields);
  return post_create_sanity_check(result, size);
}

value_t new_heap_instance_manager(runtime_t *runtime, value_t display_name) {
  size_t size = kInstanceManagerSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, instance_manager_species)));
  set_instance_manager_display_name(result, display_name);
  return post_create_sanity_check(result, size);
}

value_t new_heap_void_p(runtime_t *runtime, void *value) {
  size_t size = kVoidPSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, void_p_species)));
  set_void_p_value(result, value);
  return post_create_sanity_check(result, size);
}

value_t new_heap_factory(runtime_t *runtime, factory_constructor_t *constr) {
  TRY_DEF(constr_wrapper, new_heap_void_p(runtime, (void*) (intptr_t) constr));
  size_t size = kFactorySize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, factory_species)));
  set_factory_constructor(result, constr_wrapper);
  return post_create_sanity_check(result, size);
}

value_t new_heap_code_block(runtime_t *runtime, value_t bytecode,
    value_t value_pool, size_t high_water_mark) {
  size_t size = kCodeBlockSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_code_block_species)));
  set_code_block_bytecode(result, bytecode);
  set_code_block_value_pool(result, value_pool);
  set_code_block_high_water_mark(result, high_water_mark);
  TRY(ensure_frozen(runtime, result));
  return post_create_sanity_check(result, size);
}

value_t new_heap_type(runtime_t *runtime, alloc_flags_t flags,
    value_t raw_origin, value_t display_name) {
  size_t size = kTypeSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_type_species)));
  set_type_raw_origin(result, raw_origin);
  set_type_display_name(result, display_name);
  TRY(post_process_result(runtime, result, flags));
  return post_create_sanity_check(result, size);
}

value_t new_heap_function(runtime_t *runtime, alloc_flags_t flags,
    value_t display_name) {
  size_t size = kFunctionSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_function_species)));
  set_function_display_name(result, display_name);
  TRY(post_process_result(runtime, result, flags));
  return post_create_sanity_check(result, size);
}

value_t new_heap_argument_map_trie(runtime_t *runtime, value_t value) {
  CHECK_FAMILY(ofArray, value);
  TRY_DEF(children, new_heap_array_buffer(runtime, 2));
  size_t size = kArgumentMapTrieSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_argument_map_trie_species)));
  set_argument_map_trie_value(result, value);
  set_argument_map_trie_children(result, children);
  return post_create_sanity_check(result, size);
}

value_t new_heap_lambda(runtime_t *runtime, value_t methods, value_t captures) {
  size_t size = kLambdaSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_lambda_species)));
  set_lambda_methods(result, methods);
  set_lambda_captures(result, captures);
  return post_create_sanity_check(result, size);
}

value_t new_heap_block(runtime_t *runtime, value_t section) {
  size_t size = kBlockSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_block_species)));
  set_block_section(result, section);
  return post_create_sanity_check(result, size);
}

value_t new_heap_namespace(runtime_t *runtime, value_t value) {
  TRY_DEF(bindings, new_heap_id_hash_map(runtime, 16));
  size_t size = kNamespaceSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_namespace_species)));
  set_namespace_bindings(result, bindings);
  set_namespace_value(result, value);
  return post_create_sanity_check(result, size);
}

value_t new_heap_module_fragment(runtime_t *runtime, value_t module, value_t stage,
    value_t nspace, value_t methodspace, value_t imports) {
  CHECK_FAMILY_OPT(ofModule, module);
  CHECK_PHYLUM(tpStageOffset, stage);
  CHECK_FAMILY_OPT(ofNamespace, nspace);
  CHECK_FAMILY_OPT(ofMethodspace, methodspace);
  CHECK_FAMILY_OPT(ofIdHashMap, imports);
  TRY_DEF(phrivate, new_heap_module_fragment_private(runtime, nothing()));
  size_t size = kModuleFragmentSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_module_fragment_species)));
  set_module_fragment_stage(result, stage);
  set_module_fragment_module(result, module);
  set_module_fragment_namespace(result, nspace);
  set_module_fragment_methodspace(result, methodspace);
  set_module_fragment_imports(result, imports);
  set_module_fragment_epoch(result, feUnbound);
  set_module_fragment_private(result, phrivate);
  set_module_fragment_private_owner(phrivate, result);
  set_module_fragment_methodspaces_cache(result, nothing());
  return post_create_sanity_check(result, size);
}

value_t new_heap_module_fragment_private(runtime_t *runtime, value_t owner) {
  size_t size = kModuleFragmentPrivateSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
        ROOT(runtime, mutable_module_fragment_private_species)));
  set_module_fragment_private_owner(result, owner);
  return post_create_sanity_check(result, size);
}

value_t new_heap_empty_module(runtime_t *runtime, value_t path) {
  TRY_DEF(fragments, new_heap_array_buffer(runtime, 16));
  size_t size = kModuleSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_module_species)));
  set_module_path(result, path);
  set_module_fragments(result, fragments);
  return result;

}

value_t new_heap_operation(runtime_t *runtime, alloc_flags_t flags,
    operation_type_t type, value_t value) {
  size_t size = kOperationSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_operation_species)));
  set_operation_type(result, type);
  set_operation_value(result, value);
  TRY(post_process_result(runtime, result, flags));
  return post_create_sanity_check(result, size);
}

value_t new_heap_path(runtime_t *runtime, alloc_flags_t flags, value_t head,
    value_t tail) {
  size_t size = kPathSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_path_species)));
  set_path_raw_head(result, head);
  set_path_raw_tail(result, tail);
  TRY(post_process_result(runtime, result, flags));
  return post_create_sanity_check(result, size);
}

value_t new_heap_path_with_names(runtime_t *runtime, value_t names,
    size_t offset) {
  size_t length = get_array_length(names);
  if (offset == length)
    return ROOT(runtime, empty_path);
  TRY_DEF(tail, new_heap_path_with_names(runtime, names, offset + 1));
  value_t head = get_array_at(names, offset);
  return new_heap_path(runtime, afMutable, head, tail);
}

value_t new_heap_unknown(runtime_t *runtime, value_t header, value_t payload) {
  size_t size = kUnknownSize;
  TRY_DEF(result, alloc_heap_object(runtime, size, ROOT(runtime, unknown_species)));
  set_unknown_header(result, header);
  set_unknown_payload(result, payload);
  return post_create_sanity_check(result, size);
}

value_t new_heap_options(runtime_t *runtime, value_t elements) {
  size_t size = kOptionsSize;
  TRY_DEF(result, alloc_heap_object(runtime, size, ROOT(runtime, options_species)));
  set_options_elements(result, elements);
  return post_create_sanity_check(result, size);
}

value_t new_heap_empty_module_loader(runtime_t *runtime) {
  TRY_DEF(modules, new_heap_id_hash_map(runtime, 16));
  size_t size = kModuleLoaderSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, module_loader_species)));
  set_module_loader_modules(result, modules);
  return post_create_sanity_check(result, size);
}

value_t new_heap_unbound_module(runtime_t *runtime, value_t path, value_t fragments) {
  size_t size = kUnboundModuleSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, unbound_module_species)));
  set_unbound_module_path(result, path);
  set_unbound_module_fragments(result, fragments);
  return post_create_sanity_check(result, size);
}

value_t new_heap_unbound_module_fragment(runtime_t *runtime, value_t stage,
    value_t imports, value_t elements) {
  size_t size = kUnboundModuleFragmentSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, unbound_module_fragment_species)));
  set_unbound_module_fragment_stage(result, stage);
  set_unbound_module_fragment_imports(result, imports);
  set_unbound_module_fragment_elements(result, elements);
  return post_create_sanity_check(result, size);
}

value_t new_heap_library(runtime_t *runtime, value_t display_name, value_t modules) {
  size_t size = kLibrarySize;
  TRY_DEF(result, alloc_heap_object(runtime, size, ROOT(runtime, library_species)));
  set_library_display_name(result, display_name);
  set_library_modules(result, modules);
  return post_create_sanity_check(result, size);
}

value_t new_heap_decimal_fraction(runtime_t *runtime, value_t numerator,
    value_t denominator, value_t precision) {
  size_t size = kDecimalFractionSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, decimal_fraction_species)));
  set_decimal_fraction_numerator(result, numerator);
  set_decimal_fraction_denominator(result, denominator);
  set_decimal_fraction_precision(result, precision);
  return post_create_sanity_check(result, size);
}

value_t new_heap_global_field(runtime_t *runtime, value_t display_name) {
  size_t size = kGlobalFieldSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, global_field_species)));
  set_global_field_display_name(result, display_name);
  return post_create_sanity_check(result, size);
}

value_t new_heap_ambience(runtime_t *runtime) {
  size_t size = kAmbienceSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, ambience_species)));
  set_ambience_runtime(result, runtime);
  set_ambience_present_core_fragment(result, nothing());
  return post_create_sanity_check(result, size);
}


// --- P r o c e s s ---

value_t new_heap_stack_piece(runtime_t *runtime, size_t user_capacity,
    value_t previous, value_t stack) {
  CHECK_FAMILY_OPT(ofStackPiece, previous);
  CHECK_FAMILY_OPT(ofStack, stack);
  // Make room for the lid frame.
  size_t full_capacity = user_capacity + kFrameHeaderSize;
  size_t size = calc_stack_piece_size(full_capacity);
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, stack_piece_species)));
  set_stack_piece_capacity(result, new_integer(full_capacity));
  set_stack_piece_previous(result, previous);
  set_stack_piece_stack(result, stack);
  set_stack_piece_lid_frame_pointer(result, nothing());
  value_t *storage = get_stack_piece_storage(result);
  for (size_t i = 0; i < full_capacity; i++)
    storage[i] = nothing();
  frame_t bottom = frame_empty();
  bottom.stack_piece = result;
  bottom.frame_pointer = storage;
  bottom.stack_pointer = storage;
  bottom.limit_pointer = storage;
  bottom.flags = new_flag_set(ffSynthetic | ffStackPieceEmpty);
  bottom.pc = 0;
  close_frame(&bottom);
  return post_create_sanity_check(result, size);
}

// Pushes an artificial bottom frame onto the stack such that at the end of
// the execution we bottom out at a well-defined place and all instructions
// (particularly return) can assume that there's at least one frame below them.
static void push_stack_bottom_frame(runtime_t *runtime, value_t stack) {
  value_t code_block = ROOT(runtime, stack_bottom_code_block);
  frame_t bottom = open_stack(stack);
  bool pushed = try_push_new_frame(&bottom,
      get_code_block_high_water_mark(code_block), ffSynthetic | ffStackBottom,
      false);
  CHECK_TRUE("pushing bottom frame", pushed);
  frame_set_code_block(&bottom, code_block);
  close_frame(&bottom);
}

value_t new_heap_stack(runtime_t *runtime, size_t default_piece_capacity) {
  size_t size = kStackSize;
  TRY_DEF(piece, new_heap_stack_piece(runtime, default_piece_capacity, nothing(),
      nothing()));
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, stack_species)));
  set_stack_piece_stack(piece, result);
  set_stack_top_piece(result, piece);
  set_stack_default_piece_capacity(result, default_piece_capacity);
  set_stack_top_barrier(result, nothing());
  push_stack_bottom_frame(runtime, result);
  return post_create_sanity_check(result, size);
}

value_t new_heap_escape(runtime_t *runtime, value_t section) {
  size_t size = kEscapeSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, escape_species)));
  set_escape_section(result, section);
  return post_create_sanity_check(result, size);
}

value_t new_heap_backtrace(runtime_t *runtime, value_t entries) {
  size_t size = kBacktraceSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, backtrace_species)));
  set_backtrace_entries(result, entries);
  return post_create_sanity_check(result, size);
}

value_t new_heap_backtrace_entry(runtime_t *runtime, value_t invocation,
    value_t opcode) {
  size_t size = kBacktraceEntrySize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, backtrace_entry_species)));
  set_backtrace_entry_invocation(result, invocation);
  set_backtrace_entry_opcode(result, opcode);
  return post_create_sanity_check(result, size);
}


// --- M e t h o d ---

value_t new_heap_guard(runtime_t *runtime, alloc_flags_t flags, guard_type_t type,
    value_t value) {
  size_t size = kGuardSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_guard_species)));
  set_guard_type(result, type);
  set_guard_value(result, value);
  TRY(post_process_result(runtime, result, flags));
  return post_create_sanity_check(result, size);
}

value_t new_heap_signature(runtime_t *runtime, alloc_flags_t flags, value_t tags,
    size_t param_count, size_t mandatory_count, bool allow_extra) {
  CHECK_FAMILY_OPT(ofArray, tags);
  size_t size = kSignatureSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_signature_species)));
  set_signature_tags(result, tags);
  set_signature_parameter_count(result, param_count);
  set_signature_mandatory_count(result, mandatory_count);
  set_signature_allow_extra(result, allow_extra);
  TRY(post_process_result(runtime, result, flags));
  return post_create_sanity_check(result, size);
}

value_t new_heap_parameter(runtime_t *runtime, alloc_flags_t flags, value_t guard,
    value_t tags, bool is_optional, size_t index) {
  CHECK_FAMILY_OPT(ofGuard, guard);
  CHECK_FAMILY_OPT(ofArray, tags);
  size_t size = kParameterSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_parameter_species)));
  set_parameter_guard(result, guard);
  set_parameter_tags(result, tags);
  set_parameter_is_optional(result, is_optional);
  set_parameter_index(result, index);
  TRY(post_process_result(runtime, result, flags));
  return post_create_sanity_check(result, size);
}

value_t new_heap_signature_map(runtime_t *runtime) {
  size_t size = kSignatureMapSize;
  TRY_DEF(entries, new_heap_array_buffer(runtime, kMethodArrayInitialSize));
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_signature_map_species)));
  set_signature_map_entries(result, entries);
  return post_create_sanity_check(result, size);
}

value_t new_heap_methodspace(runtime_t *runtime) {
  size_t size = kMethodspaceSize;
  TRY_DEF(inheritance, new_heap_id_hash_map(runtime, kInheritanceMapInitialSize));
  TRY_DEF(methods, new_heap_signature_map(runtime));
  TRY_DEF(imports, new_heap_array_buffer(runtime, kImportsArrayInitialSize));
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_methodspace_species)));
  set_methodspace_inheritance(result, inheritance);
  set_methodspace_methods(result, methods);
  set_methodspace_imports(result, imports);
  return post_create_sanity_check(result, size);
}

value_t new_heap_method(runtime_t *runtime, alloc_flags_t alloc_flags,
    value_t signature, value_t syntax, value_t code, value_t fragment,
    value_t method_flags) {
  CHECK_FAMILY_OPT(ofSignature, signature);
  CHECK_FAMILY_OPT(ofCodeBlock, code);
  CHECK_PHYLUM(tpFlagSet, method_flags);
  size_t size = kMethodSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_method_species)));
  set_method_signature(result, signature);
  set_method_code(result, code);
  set_method_syntax(result, syntax);
  set_method_module_fragment(result, fragment);
  set_method_flags(result, method_flags);
  TRY(post_process_result(runtime, result, alloc_flags));
  return post_create_sanity_check(result, size);
}

value_t new_heap_call_tags(runtime_t *runtime, alloc_flags_t flags,
    value_t entries) {
  size_t size = kCallTagsSize;
  CHECK_TRUE("unsorted argument array", is_pair_array_sorted(entries));
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_call_tags_species)));
  set_call_tags_entries(result, entries);
  TRY(post_process_result(runtime, result, flags));
  return post_create_sanity_check(result, size);
}

value_t new_heap_builtin_marker(runtime_t *runtime, value_t name) {
  size_t size = kBuiltinMarkerSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, builtin_marker_species)));
  set_builtin_marker_name(result, name);
  return post_create_sanity_check(result, size);
}

value_t new_heap_builtin_implementation(runtime_t *runtime, alloc_flags_t flags,
    value_t name, value_t code, size_t posc, value_t method_flags) {
  size_t size = kBuiltinImplementationSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_builtin_implementation_species)));
  set_builtin_implementation_name(result, name);
  set_builtin_implementation_code(result, code);
  set_builtin_implementation_argument_count(result, posc);
  set_builtin_implementation_method_flags(result, method_flags);
  TRY(post_process_result(runtime, result, flags));
  return post_create_sanity_check(result, size);
}


// --- S y n t a x ---

value_t new_heap_literal_ast(runtime_t *runtime, value_t value) {
  size_t size = kLiteralAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, literal_ast_species)));
  set_literal_ast_value(result, value);
  return post_create_sanity_check(result, size);
}

value_t new_heap_array_ast(runtime_t *runtime, value_t elements) {
  size_t size = kArrayAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, array_ast_species)));
  set_array_ast_elements(result, elements);
  return post_create_sanity_check(result, size);
}

value_t new_heap_invocation_ast(runtime_t *runtime, value_t arguments) {
  size_t size = kInvocationAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, invocation_ast_species)));
  set_invocation_ast_arguments(result, arguments);
  return post_create_sanity_check(result, size);
}

value_t new_heap_signal_ast(runtime_t *runtime, value_t escape, value_t arguments,
    value_t defawlt) {
  size_t size = kSignalAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, signal_ast_species)));
  set_signal_ast_escape(result, escape);
  set_signal_ast_arguments(result, arguments);
  set_signal_ast_default(result, defawlt);
  return post_create_sanity_check(result, size);
}

value_t new_heap_signal_handler_ast(runtime_t *runtime, value_t body,
    value_t handlers) {
  size_t size = kSignalHandlerAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, signal_handler_ast_species)));
  set_signal_handler_ast_body(result, body);
  set_signal_handler_ast_handlers(result, handlers);
  return post_create_sanity_check(result, size);
}

value_t new_heap_ensure_ast(runtime_t *runtime, value_t body, value_t on_exit) {
  size_t size = kEnsureAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, ensure_ast_species)));
  set_ensure_ast_body(result, body);
  set_ensure_ast_on_exit(result, on_exit);
  return post_create_sanity_check(result, size);
}

value_t new_heap_argument_ast(runtime_t *runtime, value_t tag, value_t value) {
  size_t size = kArgumentAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, argument_ast_species)));
  set_argument_ast_tag(result, tag);
  set_argument_ast_value(result, value);
  return post_create_sanity_check(result, size);
}

value_t new_heap_sequence_ast(runtime_t *runtime, value_t values) {
  size_t size = kSequenceAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, sequence_ast_species)));
  set_sequence_ast_values(result, values);
  return post_create_sanity_check(result, size);
}

value_t new_heap_local_declaration_ast(runtime_t *runtime, value_t symbol,
    value_t is_mutable, value_t value, value_t body) {
  size_t size = kLocalDeclarationAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, local_declaration_ast_species)));
  set_local_declaration_ast_symbol(result, symbol);
  set_local_declaration_ast_is_mutable(result, is_mutable);
  set_local_declaration_ast_value(result, value);
  set_local_declaration_ast_body(result, body);
  return post_create_sanity_check(result, size);
}

value_t new_heap_block_ast(runtime_t *runtime, value_t symbol,
    value_t methods, value_t body) {
  size_t size = kBlockAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, block_ast_species)));
  set_block_ast_symbol(result, symbol);
  set_block_ast_methods(result, methods);
  set_block_ast_body(result, body);
  return post_create_sanity_check(result, size);
}

value_t new_heap_with_escape_ast(runtime_t *runtime, value_t symbol,
    value_t body) {
  size_t size = kWithEscapeAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, with_escape_ast_species)));
  set_with_escape_ast_symbol(result, symbol);
  set_with_escape_ast_body(result, body);
  return post_create_sanity_check(result, size);
}

value_t new_heap_local_variable_ast(runtime_t *runtime, value_t symbol) {
  size_t size = kLocalVariableAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, local_variable_ast_species)));
  set_local_variable_ast_symbol(result, symbol);
  return post_create_sanity_check(result, size);
}

value_t new_heap_variable_assignment_ast(runtime_t *runtime, value_t target,
    value_t value) {
  size_t size = kVariableAssignmentAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, variable_assignment_ast_species)));
  set_variable_assignment_ast_target(result, target);
  set_variable_assignment_ast_value(result, value);
  return post_create_sanity_check(result, size);
}


value_t new_heap_namespace_variable_ast(runtime_t *runtime, value_t ident) {
  size_t size = kNamespaceVariableAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, namespace_variable_ast_species)));
  set_namespace_variable_ast_identifier(result, ident);
  return post_create_sanity_check(result, size);
}

value_t new_heap_symbol_ast(runtime_t *runtime, value_t name, value_t origin) {
  size_t size = kSymbolAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, symbol_ast_species)));
  set_symbol_ast_name(result, name);
  set_symbol_ast_origin(result, origin);
  return post_create_sanity_check(result, size);
}

value_t new_heap_lambda_ast(runtime_t *runtime, value_t methods) {
  size_t size = kLambdaAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, lambda_ast_species)));
  set_lambda_ast_methods(result, methods);
  return post_create_sanity_check(result, size);
}

value_t new_heap_parameter_ast(runtime_t *runtime, value_t symbol, value_t tags,
    value_t guard) {
  size_t size = kParameterAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, parameter_ast_species)));
  set_parameter_ast_symbol(result, symbol);
  set_parameter_ast_tags(result, tags);
  set_parameter_ast_guard(result, guard);
  return post_create_sanity_check(result, size);
}

value_t new_heap_guard_ast(runtime_t *runtime, guard_type_t type, value_t value) {
  size_t size = kGuardAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, guard_ast_species)));
  set_guard_ast_type(result, type);
  set_guard_ast_value(result, value);
  return post_create_sanity_check(result, size);
}

value_t new_heap_signature_ast(runtime_t *runtime, value_t parameters,
    value_t allow_extra) {
  size_t size = kSignatureAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, signature_ast_species)));
  set_signature_ast_parameters(result, parameters);
  set_signature_ast_allow_extra(result, allow_extra);
  return post_create_sanity_check(result, size);
}

value_t new_heap_method_ast(runtime_t *runtime, value_t signature, value_t body) {
  size_t size = kMethodAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, mutable_method_ast_species)));
  set_method_ast_signature(result, signature);
  set_method_ast_body(result, body);
  return post_create_sanity_check(result, size);
}

value_t new_heap_program_ast(runtime_t *runtime, value_t entry_point,
    value_t module) {
  size_t size = kProgramAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size, ROOT(runtime, program_ast_species)));
  set_program_ast_entry_point(result, entry_point);
  set_program_ast_module(result, module);
  return post_create_sanity_check(result, size);
}

value_t new_heap_identifier(runtime_t *runtime, value_t stage, value_t path) {
  CHECK_PHYLUM_OPT(tpStageOffset, stage);
  CHECK_FAMILY_OPT(ofPath, path);
  size_t size = kIdentifierSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, identifier_species)));
  set_identifier_stage(result, stage);
  set_identifier_path(result, path);
  return post_create_sanity_check(result, size);
}

value_t new_heap_namespace_declaration_ast(runtime_t *runtime, value_t annotations,
    value_t path, value_t value) {
  CHECK_FAMILY_OPT(ofPath, path);
  CHECK_FAMILY_OPT(ofArray, annotations);
  size_t size = kNamespaceDeclarationAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, namespace_declaration_ast_species)));
  set_namespace_declaration_ast_path(result, path);
  set_namespace_declaration_ast_value(result, value);
  set_namespace_declaration_ast_annotations(result, annotations);
  return post_create_sanity_check(result, size);
}

value_t new_heap_method_declaration_ast(runtime_t *runtime, value_t annotations,
    value_t method) {
  size_t size = kMethodDeclarationAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, method_declaration_ast_species)));
  set_method_declaration_ast_annotations(result, annotations);
  set_method_declaration_ast_method(result, method);
  return post_create_sanity_check(result, size);
}

value_t new_heap_is_declaration_ast(runtime_t *runtime, value_t subtype,
    value_t supertype) {
  size_t size = kIsDeclarationAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, is_declaration_ast_species)));
  set_is_declaration_ast_subtype(result, subtype);
  set_is_declaration_ast_supertype(result, supertype);
  return post_create_sanity_check(result, size);
}

value_t new_heap_current_module_ast(runtime_t *runtime) {
  size_t size = kCurrentModuleAstSize;
  TRY_DEF(result, alloc_heap_object(runtime, size,
      ROOT(runtime, current_module_ast_species)));
  return post_create_sanity_check(result, size);
}


// --- M i s c ---

value_t alloc_heap_object(runtime_t *runtime, size_t bytes, value_t species) {
  address_t addr = NULL;
  if (runtime->gc_fuzzer != NULL) {
    if (gc_fuzzer_tick(runtime->gc_fuzzer))
      return new_heap_exhausted_condition(bytes);
  }
  if (!heap_try_alloc(&runtime->heap, bytes, &addr))
    return new_heap_exhausted_condition(bytes);
  value_t result = new_heap_object(addr);
  set_heap_object_header(result, species);
  return result;
}

static value_t extend_id_hash_map(runtime_t *runtime, value_t map) {
  // Create the new entry array first so that if it fails we bail out asap.
  size_t old_capacity = get_id_hash_map_capacity(map);
  size_t new_capacity = old_capacity * 2;
  TRY_DEF(new_entry_array, new_heap_id_hash_map_entry_array(runtime, new_capacity));
  // Capture the relevant old state in an iterator before resetting the map.
  id_hash_map_iter_t iter;
  id_hash_map_iter_init(&iter, map);
  // Reset the map.
  set_id_hash_map_capacity(map, new_capacity);
  set_id_hash_map_size(map, 0);
  set_id_hash_map_occupied_count(map, 0);
  set_id_hash_map_entry_array(map, new_entry_array);
  // Scan through and add the old data.
  while (id_hash_map_iter_advance(&iter)) {
    value_t key;
    value_t value;
    id_hash_map_iter_get_current(&iter, &key, &value);
    value_t extension = try_set_id_hash_map_at(map, key, value, false);
    // Since we were able to successfully add these pairs to the old smaller
    // map it can't fail this time around.
    CHECK_FALSE("rehashing failed", is_condition(extension));
  }
  return success();
}

value_t set_id_hash_map_at(runtime_t *runtime, value_t map, value_t key, value_t value) {
  value_t first_try = try_set_id_hash_map_at(map, key, value, false);
  if (in_condition_cause(ccMapFull, first_try)) {
    TRY(extend_id_hash_map(runtime, map));
    value_t second_try = try_set_id_hash_map_at(map, key, value, false);
    // It should be impossible for the second try to fail if the first try could
    // hash the key and extending was successful.
    CHECK_FALSE("second try failure", is_condition(second_try));
    return second_try;
  } else {
    return first_try;
  }
}

value_t set_instance_field(runtime_t *runtime, value_t instance, value_t key,
    value_t value) {
  value_t fields = get_instance_fields(instance);
  return set_id_hash_map_at(runtime, fields, key, value);
}
