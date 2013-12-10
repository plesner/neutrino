// Copyright 2013 the Neutrino authors (see AUTHORS).
// Licensed under the Apache License, Version 2.0 (see LICENSE).

#include "behavior.h"
#include "builtin.h"
#include "runtime.h"
#include "tagged.h"
#include "utils.h"
#include "value-inl.h"

#include <math.h>

// --- S t a g e   o f f s e t ---

void stage_offset_print_on(value_t value, string_buffer_t *buf, print_flags_t flags) {
  int32_t offset = get_stage_offset_value(value);
  char c;
  if (offset < 0) {
    c = '@';
    offset = -offset;
  } else {
    c = '$';
    offset += 1;
  }
  for (int32_t i = 0; i < offset; i++)
    string_buffer_putc(buf, c);
}

value_t stage_offset_ordering_compare(value_t a, value_t b) {
  CHECK_PHYLUM(tpStageOffset, a);
  CHECK_PHYLUM(tpStageOffset, b);
  int32_t a_stage = get_stage_offset_value(a);
  int32_t b_stage = get_stage_offset_value(b);
  return compare_signed_integers(a_stage, b_stage);
}


// --- N o t h i n g ---

void nothing_print_on(value_t value, string_buffer_t *buf, print_flags_t flags) {
  string_buffer_printf(buf, "#<nothing>");
}


// --- N u l l ---

GET_FAMILY_PRIMARY_TYPE_IMPL(null);
NO_BUILTIN_METHODS(null);

void null_print_on(value_t value, string_buffer_t *buf, print_flags_t flags) {
  string_buffer_printf(buf, "null");
}


// --- B o o l e a n ---

GET_FAMILY_PRIMARY_TYPE_IMPL(boolean);
NO_BUILTIN_METHODS(boolean);

void boolean_print_on(value_t value, string_buffer_t *buf, print_flags_t flags) {
  string_buffer_printf(buf, get_boolean_value(value) ? "true" : "false");
}

value_t boolean_ordering_compare(value_t a, value_t b) {
  CHECK_PHYLUM(tpBoolean, a);
  CHECK_PHYLUM(tpBoolean, b);
  return compare_signed_integers(get_boolean_value(a), get_boolean_value(b));
}


// --- R e l a t i o n ---

void relation_print_on(value_t value, string_buffer_t *buf, print_flags_t flags) {
  const char *str = NULL;
  switch (get_relation_value(value)) {
    case reLessThan:
      str = "<";
      break;
    case reEqual:
      str = "==";
      break;
    case reGreaterThan:
      str = ">";
      break;
    case reUnordered:
      str = "?";
      break;
  }
  string_buffer_printf(buf, str);
}


// --- F l o a t   3 2 ---

GET_FAMILY_PRIMARY_TYPE_IMPL(float_32);

void float_32_print_on(value_t value, string_buffer_t *buf, print_flags_t flags) {
  string_buffer_printf(buf, "%f", get_float_32_value(value));
}

value_t float_32_ordering_compare(value_t a, value_t b) {
  CHECK_PHYLUM(tpFloat32, a);
  CHECK_PHYLUM(tpFloat32, b);
  float a_value = get_float_32_value(a);
  float b_value = get_float_32_value(b);
  return new_relation(compare_float_32(a_value, b_value));
}

relation_t compare_float_32(float32_t a, float32_t b) {
  if (a < b) {
    return reLessThan;
  } else if (b < a) {
    return reGreaterThan;
  } else if (a == b) {
    return reEqual;
  } else {
    return reUnordered;
  }
}

value_t float_32_infinity() {
  // TODO: fix this for MSVC.
  return new_float_32(IF_MSVC(0, INFINITY));
}

value_t float_32_minus_infinity() {
  // TODO: fix this for MSVC.
  return new_float_32(IF_MSVC(0, -INFINITY));
}

value_t float_32_nan() {
  // TODO: fix this for MSVC.
  return new_float_32(IF_MSVC(0, NAN));
}

bool is_float_32_finite(value_t value) {
  // TODO: fix this for MSVC.
  return IF_MSVC(false, isfinite(get_float_32_value(value)));
}

bool is_float_32_nan(value_t value) {
  // TODO: fix this for MSVC.
  return IF_MSVC(false, isnan(get_float_32_value(value)));
}

static value_t float_32_negate(builtin_arguments_t *args) {
  value_t self = get_builtin_subject(args);
  CHECK_PHYLUM(tpFloat32, self);
  return new_float_32(-get_float_32_value(self));
}

static value_t float_32_minus_float_32(builtin_arguments_t *args) {
  value_t self = get_builtin_subject(args);
  value_t that = get_builtin_argument(args, 0);
  CHECK_PHYLUM(tpFloat32, self);
  CHECK_PHYLUM(tpFloat32, that);
  return new_float_32(get_float_32_value(self) - get_float_32_value(that));
}

static value_t float_32_plus_float_32(builtin_arguments_t *args) {
  value_t self = get_builtin_subject(args);
  value_t that = get_builtin_argument(args, 0);
  CHECK_PHYLUM(tpFloat32, self);
  CHECK_PHYLUM(tpFloat32, that);
  return new_float_32(get_float_32_value(self) + get_float_32_value(that));
}

static value_t float_32_equals_float_32(builtin_arguments_t *args) {
  value_t self = get_builtin_subject(args);
  value_t that = get_builtin_argument(args, 0);
  CHECK_PHYLUM(tpFloat32, self);
  CHECK_PHYLUM(tpFloat32, that);
  return new_boolean(test_relation(value_ordering_compare(self, that), reEqual));
}

value_t add_float_32_builtin_methods(runtime_t *runtime, safe_value_t s_space) {
  DEF_PREFIX(prefix_minus, "-");
  ADD_BUILTIN(float_32, prefix_minus, 0, float_32_negate);
  DEF_INFIX(infix_minus, "-");
  ADD_BUILTIN(float_32, infix_minus, 1, float_32_minus_float_32);
  DEF_INFIX(infix_plus, "+");
  ADD_BUILTIN(float_32, infix_plus, 1, float_32_plus_float_32);
  DEF_INFIX(infix_eqeq, "==");
  ADD_BUILTIN(float_32, infix_eqeq, 1, float_32_equals_float_32);
  return success();
}