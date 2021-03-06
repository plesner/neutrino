// Copyright 2013 the Neutrino authors (see AUTHORS).
// Licensed under the Apache License, Version 2.0 (see LICENSE).

#include "alloc.h"
#include "behavior.h"
#include "derived-inl.h"
#include "log.h"
#include "process.h"
#include "tagged-inl.h"
#include "try-inl.h"
#include "value-inl.h"


/// ## Stack piece

FIXED_GET_MODE_IMPL(stack_piece, vmMutable);

ACCESSORS_IMPL(StackPiece, stack_piece, acNoCheck, 0, Capacity, capacity);
ACCESSORS_IMPL(StackPiece, stack_piece, acInFamilyOpt, ofStackPiece, Previous, previous);
ACCESSORS_IMPL(StackPiece, stack_piece, acInFamilyOpt, ofStack, Stack, stack);
ACCESSORS_IMPL(StackPiece, stack_piece, acNoCheck, 0, LidFramePointer, lid_frame_pointer);

size_t calc_stack_piece_size(size_t capacity) {
  return kStackPieceHeaderSize + (capacity * kValueSize);
}

value_t *get_stack_piece_storage(value_t value) {
  CHECK_FAMILY(ofStackPiece, value);
  return access_heap_object_field(value, kStackPieceStorageOffset);
}

int set_stack_piece_storage(value_t self, value_t value) {
  UNREACHABLE("set_stack_piece_storage");
  return 0;
}

void get_stack_piece_layout(value_t value, heap_object_layout_t *layout) {
  size_t capacity = get_integer_value(get_stack_piece_capacity(value));
  size_t size = calc_stack_piece_size(capacity);
  heap_object_layout_set(layout, size, kHeapObjectHeaderSize);
}

value_t stack_piece_validate(value_t value) {
  VALIDATE_FAMILY(ofStackPiece, value);
  VALIDATE_FAMILY_OPT(ofStackPiece, get_stack_piece_previous(value));
  VALIDATE_FAMILY_OPT(ofStack, get_stack_piece_stack(value));
  VALIDATE_DOMAIN(vdInteger, get_stack_piece_capacity(value));
  return success();
}

void stack_piece_print_on(value_t value, print_on_context_t *context) {
  CHECK_FAMILY(ofStackPiece, value);
  string_buffer_printf(context->buf, "#<stack piece ~%w: st@%i>", value,
      get_stack_piece_capacity(value));
}

bool is_stack_piece_closed(value_t self) {
  return is_integer(get_stack_piece_lid_frame_pointer(self));
}


// --- S t a c k   ---

FIXED_GET_MODE_IMPL(stack, vmMutable);
TRIVIAL_PRINT_ON_IMPL(Stack, stack);

ACCESSORS_IMPL(Stack, stack, acInFamily, ofStackPiece, TopPiece, top_piece);
INTEGER_ACCESSORS_IMPL(Stack, stack, DefaultPieceCapacity, default_piece_capacity);
ACCESSORS_IMPL(Stack, stack, acNoCheck, 0, TopBarrier, top_barrier);

value_t stack_validate(value_t self) {
  VALIDATE_FAMILY(ofStack, self);
  VALIDATE_FAMILY(ofStackPiece, get_stack_top_piece(self));
  value_t current = get_stack_top_piece(self);
  while (!is_nothing(current)) {
    value_t stack = get_stack_piece_stack(current);
    VALIDATE(is_same_value(stack, self));
    current = get_stack_piece_previous(current);
  }
  return success();
}

// Transfers the arguments from the top of the previous piece (which the frame
// points to) to the bottom of the new stack segment.
static void transfer_top_arguments(value_t new_piece, frame_t *frame,
    size_t arg_count) {
  frame_t new_frame = frame_empty();
  open_stack_piece(new_piece, &new_frame);
  for (size_t i = 0; i < arg_count; i++) {
    value_t value = frame_peek_value(frame, arg_count - i - 1);
    frame_push_value(&new_frame, value);
  }
  close_frame(&new_frame);
}

static void push_stack_piece_bottom_frame(runtime_t *runtime, value_t stack_piece,
    value_t arg_map) {
  frame_t bottom = frame_empty();
  value_t code_block = ROOT(runtime, stack_piece_bottom_code_block);
  // The transferred arguments are going to appear as if they were arguments
  // passed from this frame so we have to "allocate" enough room for them on
  // the stack.
  open_stack_piece(stack_piece, &bottom);
  size_t arg_count = get_array_length(arg_map);
  bool pushed = try_push_new_frame(&bottom,
      get_code_block_high_water_mark(code_block) + arg_count,
      ffSynthetic | ffStackPieceBottom, false);
  CHECK_TRUE("pushing bottom frame", pushed);
  frame_set_code_block(&bottom, code_block);
  frame_set_argument_map(&bottom, arg_map);
  close_frame(&bottom);
}

// Reads the state of the stack piece lid into the given frame; doesn't modify
// the piece in any way though.
static void read_stack_piece_lid(value_t piece, frame_t *frame) {
  CHECK_TRUE("stack piece not closed", is_stack_piece_closed(piece));
  frame->stack_piece = piece;
  value_t *stack_start = frame_get_stack_piece_bottom(frame);
  frame->frame_pointer = stack_start + get_integer_value(get_stack_piece_lid_frame_pointer(piece));
  frame_walk_down_stack(frame);
}

void open_stack_piece(value_t piece, frame_t *frame) {
  CHECK_FAMILY(ofStackPiece, piece);
  read_stack_piece_lid(piece, frame);
  set_stack_piece_lid_frame_pointer(piece, nothing());
}

void close_frame(frame_t *frame) {
  value_t piece = frame->stack_piece;
  CHECK_FALSE("stack piece already closed", is_stack_piece_closed(piece));
  bool pushed = try_push_new_frame(frame, 0, ffLid | ffSynthetic, true);
  CHECK_TRUE("Failed to close frame", pushed);
  value_t *stack_start = frame_get_stack_piece_bottom(frame);
  set_stack_piece_lid_frame_pointer(piece, new_integer(frame->frame_pointer - stack_start));
  frame->stack_piece = nothing();
  frame->frame_pointer = frame->limit_pointer = frame->stack_pointer = 0;
  frame->pc = 0;
}

value_t push_stack_frame(runtime_t *runtime, value_t stack, frame_t *frame,
    size_t frame_capacity, value_t arg_map) {
  CHECK_FAMILY(ofStack, stack);
  value_t top_piece = get_stack_top_piece(stack);
  CHECK_FALSE("stack piece closed", is_stack_piece_closed(top_piece));
  if (!try_push_new_frame(frame, frame_capacity, ffOrganic, false)) {
    // There wasn't room to push this frame onto the top stack piece so
    // allocate a new top piece that definitely has room.
    size_t default_capacity = get_stack_default_piece_capacity(stack);
    size_t transfer_arg_count = get_array_length(arg_map);
    size_t required_capacity
        = frame_capacity          // the new frame's locals
        + kFrameHeaderSize        // the new frame's header
        + 1                       // the synthetic bottom frame's one local
        + kFrameHeaderSize        // the synthetic bottom frame's header
        + transfer_arg_count;     // any arguments to be copied onto the piece
    size_t new_capacity = max_size(default_capacity, required_capacity);

    // Create and initialize the new stack segment. The frame struct is still
    // pointing to the old frame.
    TRY_DEF(new_piece, new_heap_stack_piece(runtime, new_capacity, top_piece,
        stack));
    push_stack_piece_bottom_frame(runtime, new_piece, arg_map);
    transfer_top_arguments(new_piece, frame, transfer_arg_count);
    set_stack_top_piece(stack, new_piece);

    // Close the previous stack piece, recording the frame state.
    close_frame(frame);

    // Finally, create a new frame on the new stack which includes updating the
    // struct. The required_capacity calculation ensures that this call will
    // succeed.
    open_stack_piece(new_piece, frame);
    bool pushed_stack_piece = try_push_new_frame(frame, frame_capacity,
        ffOrganic, false);
    if (!pushed_stack_piece)
      try_push_new_frame(frame, frame_capacity,
              ffOrganic, false);
    CHECK_TRUE("pushing on new piece failed", pushed_stack_piece);
  }
  frame_set_argument_map(frame, arg_map);
  return success();
}

void frame_walk_down_stack(frame_t *frame) {
  frame_t snapshot = *frame;
  // Get the frame pointer and capacity from the frame's header.
  value_t *stack_start = frame_get_stack_piece_bottom(frame);
  frame->frame_pointer = stack_start + frame_get_previous_frame_pointer(&snapshot);
  frame->limit_pointer = stack_start + frame_get_previous_limit_pointer(&snapshot);
  frame->flags = frame_get_previous_flags(&snapshot);
  frame->pc = frame_get_previous_pc(&snapshot);
  // The stack pointer will be the first field of the top frame's header.
  frame->stack_pointer = snapshot.frame_pointer - kFrameHeaderSize;
}

bool frame_has_flag(frame_t *frame, frame_flag_t flag) {
  return get_flag_set_at(frame->flags, flag);
}

value_t *frame_get_stack_piece_bottom(frame_t *frame) {
  return get_stack_piece_storage(frame->stack_piece);
}

value_t *frame_get_stack_piece_top(frame_t *frame) {
  value_t *storage = get_stack_piece_storage(frame->stack_piece);
  return storage + get_integer_value(get_stack_piece_capacity(frame->stack_piece));
}

frame_t open_stack(value_t stack) {
  CHECK_FAMILY(ofStack, stack);
  frame_t result = frame_empty();
  open_stack_piece(get_stack_top_piece(stack), &result);
  return result;
}


/// ### Barrier iter

value_t barrier_iter_init(barrier_iter_t *iter, frame_t *frame) {
  value_t stack = get_stack_piece_stack(frame->stack_piece);
  return iter->current = get_stack_top_barrier(stack);
}

value_t barrier_iter_advance(barrier_iter_t *iter) {
  iter->current = get_barrier_state_previous(iter->current);
  return iter->current;
}


// --- F r a m e ---

bool try_push_new_frame(frame_t *frame, size_t frame_capacity, uint32_t flags,
    bool is_lid) {
  value_t stack_piece = frame->stack_piece;
  CHECK_FALSE("pushing closed stack piece", is_stack_piece_closed(stack_piece));
  // First record the current state of the old top frame so we can store it in
  // the header of the new frame.
  frame_t old_frame = *frame;
  // Determine how much room is left in the stack piece.
  value_t *stack_piece_start = get_stack_piece_storage(stack_piece);
  size_t capacity = get_integer_value(get_stack_piece_capacity(stack_piece));
  value_t *stack_piece_limit = stack_piece_start + capacity;
  // There must always be room on a stack piece for the lid frame because it
  // must always be possible to close a stack if a condition occurs, which we
  // assume it can at any time. So we hold back a frame header's worth of stack
  // except when allocating the lid.
  if (!is_lid)
    stack_piece_limit -= kFrameHeaderSize;
  value_t *new_frame_pointer = old_frame.stack_pointer + kFrameHeaderSize;
  value_t *new_frame_limit = new_frame_pointer + frame_capacity;
  if (new_frame_limit > stack_piece_limit)
    return false;
  // Store the new frame's info in the frame struct.
  frame->stack_pointer = frame->frame_pointer = new_frame_pointer;
  frame->limit_pointer = new_frame_limit;
  frame->flags = new_flag_set(flags);
  frame->pc = 0;
  // Record the relevant information about the previous frame in the new frame's
  // header.
  frame_set_previous_frame_pointer(frame, old_frame.frame_pointer - stack_piece_start);
  frame_set_previous_limit_pointer(frame, old_frame.limit_pointer - stack_piece_start);
  frame_set_previous_flags(frame, old_frame.flags);
  frame_set_previous_pc(frame, old_frame.pc);
  frame_set_code_block(frame, nothing());
  frame_set_argument_map(frame, nothing());
  return true;
}

void frame_pop_within_stack_piece(frame_t *frame) {
  CHECK_FALSE("popping closed stack piece",
      is_stack_piece_closed(frame->stack_piece));
  CHECK_FALSE("stack piece empty", frame_has_flag(frame, ffStackPieceEmpty));
  frame_walk_down_stack(frame);
}

// Accesses a frame header field, that is, a bookkeeping field below the frame
// pointer.
static value_t *access_frame_header_field(frame_t *frame, size_t offset) {
  CHECK_REL("frame header field out of bounds", offset, <=, kFrameHeaderSize);
  value_t *location = frame->frame_pointer - offset - 1;
  CHECK_TRUE("frame header out of bounds", frame_get_stack_piece_bottom(frame) <= location);
  return location;
}

// Returns true if the given absolute offset is within the fields available to
// the given frame.
static bool is_offset_within_frame(frame_t *frame, value_t *offset) {
  return (frame->frame_pointer <= offset) && (offset < frame->limit_pointer);
}

void frame_set_previous_frame_pointer(frame_t *frame, size_t value) {
  *access_frame_header_field(frame, kFrameHeaderPreviousFramePointerOffset) =
      new_integer(value);
}

size_t frame_get_previous_frame_pointer(frame_t *frame) {
  return get_integer_value(*access_frame_header_field(frame,
      kFrameHeaderPreviousFramePointerOffset));
}

void frame_set_previous_limit_pointer(frame_t *frame, size_t value) {
  *access_frame_header_field(frame, kFrameHeaderPreviousLimitPointerOffset) =
      new_integer(value);
}

size_t frame_get_previous_limit_pointer(frame_t *frame) {
  return get_integer_value(*access_frame_header_field(frame,
      kFrameHeaderPreviousLimitPointerOffset));
}

void frame_set_previous_flags(frame_t *frame, value_t flags) {
  *access_frame_header_field(frame, kFrameHeaderPreviousFlagsOffset) = flags;
}

value_t frame_get_previous_flags(frame_t *frame) {
  return *access_frame_header_field(frame, kFrameHeaderPreviousFlagsOffset);
}

void frame_set_code_block(frame_t *frame, value_t code_block) {
  *access_frame_header_field(frame, kFrameHeaderCodeBlockOffset) =
      code_block;
}

value_t frame_get_code_block(frame_t *frame) {
  return *access_frame_header_field(frame, kFrameHeaderCodeBlockOffset);
}

void frame_set_argument_map(frame_t *frame, value_t argument_map) {
  *access_frame_header_field(frame, kFrameHeaderArgumentMapOffset) =
      argument_map;
}

value_t frame_get_argument_map(frame_t *frame) {
  return *access_frame_header_field(frame, kFrameHeaderArgumentMapOffset);
}

void frame_set_previous_pc(frame_t *frame, size_t pc) {
  *access_frame_header_field(frame, kFrameHeaderPreviousPcOffset) =
      new_integer(pc);
}

size_t frame_get_previous_pc(frame_t *frame) {
  return get_integer_value(*access_frame_header_field(frame,
      kFrameHeaderPreviousPcOffset));
}

value_t frame_push_value(frame_t *frame, value_t value) {
  // Check that the stack is in sync with this frame.
  COND_CHECK_TRUE("push out of frame bounds", ccOutOfBounds,
      is_offset_within_frame(frame, frame->stack_pointer));
  CHECK_FALSE("pushing condition", is_condition(value));
  *(frame->stack_pointer++) = value;
  return success();
}

value_array_t frame_alloc_array(frame_t *frame, size_t size) {
  CHECK_TRUE("push out of frame bounds",
      is_offset_within_frame(frame, frame->stack_pointer + (size - 1)));
  value_t *start = frame->stack_pointer;
  frame->stack_pointer += size;
  return new_value_array(start, size);
}

value_t frame_alloc_derived_object(frame_t *frame, genus_descriptor_t *desc) {
  value_array_t memory = frame_alloc_array(frame, desc->field_count);
  value_t result = alloc_derived_object(memory, desc, frame->stack_piece);
  if (desc->on_scope_exit) {
    value_t stack = get_stack_piece_stack(frame->stack_piece);
    set_barrier_state_previous(result, get_stack_top_barrier(stack));
    set_stack_top_barrier(stack, result);
  }
  return result;
}

void frame_destroy_derived_object(frame_t *frame, genus_descriptor_t *desc) {
#ifdef EXPENSIVE_CHECKS
  value_t *anchor_ptr = frame->stack_pointer - desc->after_field_count - 1;
  value_t derived = new_derived_object((address_t) anchor_ptr);
  value_validate(derived);
#endif
  for (size_t i = 0; i < desc->field_count; i++)
    *(--frame->stack_pointer) = nothing();
}

value_t frame_pop_value(frame_t *frame) {
  COND_CHECK_TRUE("pop out of frame bounds", ccOutOfBounds,
      is_offset_within_frame(frame, frame->stack_pointer - 1));
  return *(--frame->stack_pointer);
}

value_t frame_peek_value(frame_t *frame, size_t index) {
  return frame->stack_pointer[-(index + 1)];
}

value_t frame_get_argument(frame_t *frame, size_t param_index) {
  value_t *stack_pointer = frame->frame_pointer - kFrameHeaderSize;
  value_t arg_map = frame_get_argument_map(frame);
  size_t offset = get_integer_value(get_array_at(arg_map, param_index));
  return stack_pointer[-(offset + 1)];
}

value_t frame_get_pending_argument_at(frame_t *frame, value_t tags, size_t index) {
  CHECK_FAMILY(ofCallTags, tags);
  size_t offset = get_call_tags_offset_at(tags, index);
  return frame_peek_value(frame, offset);
}

void frame_set_argument(frame_t *frame, size_t param_index, value_t value) {
  value_t *stack_pointer = frame->frame_pointer - kFrameHeaderSize;
  value_t arg_map = frame_get_argument_map(frame);
  size_t offset = get_integer_value(get_array_at(arg_map, param_index));
  stack_pointer[-(offset + 1)] = value;
}

value_t frame_get_local(frame_t *frame, size_t index) {
  value_t *location = frame->frame_pointer + index;
  COND_CHECK_TRUE("local not defined yet", ccOutOfBounds,
      location < frame->stack_pointer);
  return *location;
}


// --- F r a m e   i t e r a t o r ---

void frame_iter_init_from_frame(frame_iter_t *iter, frame_t *frame) {
  iter->current = *frame;
}

frame_t *frame_iter_get_current(frame_iter_t *iter) {
  return &iter->current;
}

bool frame_iter_advance(frame_iter_t *iter) {
  frame_t *current = &iter->current;
  do {
    // Advance the current frame to the next one.
    frame_walk_down_stack(current);
    if (frame_has_flag(current, ffStackPieceBottom)) {
      // If this is the bottom frame of a stack piece jump to the previous
      // piece.
      current->stack_piece = get_stack_piece_previous(current->stack_piece);
      read_stack_piece_lid(current->stack_piece, current);
    } else if (frame_has_flag(current, ffStackBottom)) {
      // If we're at the bottom of the stack there are no more frames.
      return false;
    }
  } while (!frame_has_flag(current, ffOrganic));
  // We must have reached an organic frame so return true.
  return true;
}


// ## Escape

FIXED_GET_MODE_IMPL(escape, vmMutable);
TRIVIAL_PRINT_ON_IMPL(Escape, escape);
GET_FAMILY_PRIMARY_TYPE_IMPL(escape);

ACCESSORS_IMPL(Escape, escape, acInGenusOpt, dgEscapeSection, Section, section);

value_t escape_validate(value_t value) {
  VALIDATE_FAMILY(ofEscape, value);
  VALIDATE_GENUS_OPT(dgEscapeSection, get_escape_section(value));
  return success();
}

static value_t emit_fire_escape(assembler_t *assm) {
  TRY(assembler_emit_fire_escape_or_barrier(assm));
  return success();
}

static value_t escape_is_live(builtin_arguments_t *args) {
  value_t self = get_builtin_subject(args);
  CHECK_FAMILY(ofEscape, self);
  return new_boolean(!is_nothing(get_escape_section(self)));
}

value_t add_escape_builtin_implementations(runtime_t *runtime, safe_value_t s_map) {
  TRY(add_custom_method_impl(runtime, deref(s_map), "escape()", 1,
      new_flag_set(kFlagSetAllOff), emit_fire_escape));
  ADD_BUILTIN_IMPL("escape.is_live", 0, escape_is_live);
  return success();
}


// ## Lambda

GET_FAMILY_PRIMARY_TYPE_IMPL(lambda);

ACCESSORS_IMPL(Lambda, lambda, acInFamilyOpt, ofMethodspace, Methods, methods);
ACCESSORS_IMPL(Lambda, lambda, acInFamilyOpt, ofArray, Captures, captures);

value_t lambda_validate(value_t self) {
  VALIDATE_FAMILY(ofLambda, self);
  VALIDATE_FAMILY_OPT(ofMethodspace, get_lambda_methods(self));
  VALIDATE_FAMILY_OPT(ofArray, get_lambda_captures(self));
  return success();
}

void lambda_print_on(value_t value, print_on_context_t *context) {
  CHECK_FAMILY(ofLambda, value);
  string_buffer_printf(context->buf, "lambda~%w", value); // Unicode lambda.
}

value_t emit_lambda_call_trampoline(assembler_t *assm) {
  TRY(assembler_emit_delegate_lambda_call(assm));
  return success();
}

value_t add_lambda_builtin_implementations(runtime_t *runtime, safe_value_t s_map) {
  TRY(add_custom_method_impl(runtime, deref(s_map), "lambda()", 0,
      new_flag_set(mfLambdaDelegate), emit_lambda_call_trampoline));
  return success();
}

value_t get_lambda_capture(value_t self, size_t index) {
  CHECK_FAMILY(ofLambda, self);
  value_t captures = get_lambda_captures(self);
  return get_array_at(captures, index);
}

value_t ensure_lambda_owned_values_frozen(runtime_t *runtime, value_t self) {
  TRY(ensure_frozen(runtime, get_lambda_captures(self)));
  return success();
}


/// ## Block

GET_FAMILY_PRIMARY_TYPE_IMPL(block);

ACCESSORS_IMPL(Block, block, acNoCheck, 0, Section, section);

value_t block_validate(value_t self) {
  VALIDATE_FAMILY(ofBlock, self);
  VALIDATE_GENUS_OPT(dgBlockSection, get_block_section(self));
  return success();
}

void block_print_on(value_t value, print_on_context_t *context) {
  CHECK_FAMILY(ofBlock, value);
  string_buffer_printf(context->buf, "block~%w", value); // Unicode beta.
}

static value_t emit_block_call_trampoline(assembler_t *assm) {
  TRY(assembler_emit_delegate_block_call(assm));
  return success();
}

static value_t block_is_live(builtin_arguments_t *args) {
  value_t self = get_builtin_subject(args);
  CHECK_FAMILY(ofBlock, self);
  return new_boolean(!is_nothing(get_block_section(self)));
}

value_t add_block_builtin_implementations(runtime_t *runtime, safe_value_t s_map) {
  TRY(add_custom_method_impl(runtime, deref(s_map), "block()", 0,
      new_flag_set(mfBlockDelegate), emit_block_call_trampoline));
  ADD_BUILTIN_IMPL("block.is_live", 0, block_is_live);
  return success();
}

// Returns the refraction point data (that is, a derived object which holds
// refraction point state) for the given value.
static value_t get_refraction_point(value_t self) {
  if (is_heap_object(self)) {
    CHECK_FAMILY(ofBlock, self);
    return get_block_section(self);
  } else {
    CHECK_DOMAIN(vdDerivedObject, self);
    return self;
  }
}

void get_refractor_refracted_frame(value_t self, size_t block_depth,
    frame_t *frame) {
  CHECK_REL("refractor not nested", block_depth, >, 0);
  value_t current = self;
  for (size_t i = block_depth; i > 0; i--) {
    // Locate the next refraction point.
    value_t refraction_point = get_refraction_point(current);
    value_t fp_val = get_refraction_point_frame_pointer(refraction_point);
    size_t fp = get_integer_value(fp_val);
    // Update the frame state to point to it.
    frame->stack_piece = get_derived_object_host(refraction_point);
    frame->frame_pointer = get_stack_piece_storage(frame->stack_piece) + fp;
    if (i > 1)
      current = frame_get_argument(frame, 0);
  }
  // We don't know the limit or stack pointers so the best estimate is that they
  // definitely don't go past the stack piece.
  frame->limit_pointer = frame_get_stack_piece_top(frame);
  frame->stack_pointer = frame_get_stack_piece_top(frame);
  // We also don't know what the flags should be so set this to nothing such
  // that trying to access them as flags fails.
  frame->flags = nothing();
}


// --- B a c k t r a c e ---

FIXED_GET_MODE_IMPL(backtrace, vmMutable);
GET_FAMILY_PRIMARY_TYPE_IMPL(backtrace);
NO_BUILTIN_METHODS(backtrace);

ACCESSORS_IMPL(Backtrace, backtrace, acInFamily, ofArrayBuffer, Entries, entries);

value_t backtrace_validate(value_t value) {
  VALIDATE_FAMILY(ofBacktrace, value);
  return success();
}

void backtrace_print_on(value_t value, print_on_context_t *context) {
  CHECK_FAMILY(ofBacktrace, value);
  string_buffer_printf(context->buf, "--- backtrace ---");
  value_t entries = get_backtrace_entries(value);
  for (size_t i = 0; i < get_array_buffer_length(entries); i++) {
    string_buffer_putc(context->buf, '\n');
    string_buffer_printf(context->buf, "- ");
    value_print_inner_on(get_array_buffer_at(entries, i), context, -1);
  }
}

value_t capture_backtrace(runtime_t *runtime, frame_t *top) {
  TRY_DEF(frames, new_heap_array_buffer(runtime, 16));
  frame_iter_t iter;
  frame_iter_init_from_frame(&iter, top);
  do {
    frame_t *frame = frame_iter_get_current(&iter);
    TRY_DEF(entry, capture_backtrace_entry(runtime, frame));
    if (!is_nothing(entry))
      TRY(add_to_array_buffer(runtime, frames, entry));
  } while (frame_iter_advance(&iter));
  return new_heap_backtrace(runtime, frames);
}


// --- B a c k t r a c e   e n t r y ---

FIXED_GET_MODE_IMPL(backtrace_entry, vmMutable);

ACCESSORS_IMPL(BacktraceEntry, backtrace_entry, acNoCheck, 0, Invocation,
    invocation);
ACCESSORS_IMPL(BacktraceEntry, backtrace_entry, acNoCheck, 0, Opcode,
    opcode);

value_t backtrace_entry_validate(value_t value) {
  VALIDATE_FAMILY(ofBacktraceEntry, value);
  return success();
}

void backtrace_entry_invocation_print_on(value_t invocation, int32_t opcode,
    print_on_context_t *context) {
  // Ensure entries don't have arguments etc. so get them out of the way first.
  if (opcode == ocCallEnsurer) {
    string_buffer_printf(context->buf, "ensure");
    return;
  }
  value_t subject = new_not_found_condition();
  value_t selector = new_not_found_condition();
  id_hash_map_iter_t iter;
  id_hash_map_iter_init(&iter, invocation);
  while (id_hash_map_iter_advance(&iter)) {
    value_t key;
    value_t value;
    id_hash_map_iter_get_current(&iter, &key, &value);
    if (in_family(ofKey, key)) {
      size_t id = get_key_id(key);
      if (id == 0)
        subject = value;
      else if (id == 1)
        selector = value;
    }
  }
  // Print the subject as the first thing. For aborts we ignore the subject
  // (which is not supposed to be there anyway) and just print abort.
  if (opcode == ocSignalEscape || opcode == ocBuiltinMaybeEscape) {
    string_buffer_printf(context->buf, "leave");
  } else if (opcode == ocSignalContinue) {
    string_buffer_printf(context->buf, "signal");
  } else if (!in_condition_cause(ccNotFound, subject)) {
    value_print_inner_on(subject, context, -1);
  }
  // Begin the selector.
  if (in_family(ofOperation, selector)) {
    operation_print_open_on(selector, context);
  } else if (!in_condition_cause(ccNotFound, selector)) {
    value_print_inner_on(selector, context, -1);
  }
  // Number of positional arguments.
  size_t posc;
  // Number of arguments in total discounting the subject and selector.
  size_t argc;
  // Print the positional arguments.
  for (posc = argc = 0; true; posc++, argc++) {
    value_t key = new_integer(posc);
    value_t value = get_id_hash_map_at(invocation, key);
    if (in_condition_cause(ccNotFound, value))
      break;
    if (argc > 0)
      string_buffer_printf(context->buf, ", ");
    value_print_inner_on(value, context, -1);
  }
  // Print any remaining arguments. Note that this will print them in
  // nondeterministic order since the order depends on the iteration order of
  // the map. This is bad.
  id_hash_map_iter_init(&iter, invocation);
  while (id_hash_map_iter_advance(&iter)) {
    value_t key;
    value_t value;
    id_hash_map_iter_get_current(&iter, &key, &value);
    if (in_family(ofKey, key)) {
      size_t id = get_key_id(key);
      if (id == 0 || id == 1)
        // Don't print the subject/selector again.
        continue;
    } else if (is_integer(key) && ((size_t) get_integer_value(key)) < posc) {
      // Don't print any of the positional arguments again. The size_t cast of
      // the integer value means that negative values will become very large
      // positive ones and hence compare greater than posc.
      continue;
    }
    if (argc > 0)
      string_buffer_printf(context->buf, ", ");
    // Unquote the value such that string tags are unquoted as you would expect.
    print_on_context_t new_context = *context;
    new_context.flags = pfUnquote;
    value_print_inner_on(key, &new_context, -1);
    string_buffer_printf(context->buf, ": ");
    value_print_inner_on(value, context, -1);
    argc++;
  }
  // End the selector.
  if (in_family(ofOperation, selector))
    operation_print_close_on(selector, context);
}

void backtrace_entry_print_on(value_t value, print_on_context_t *context) {
  CHECK_FAMILY(ofBacktraceEntry, value);
  value_t invocation = get_backtrace_entry_invocation(value);
  int32_t opcode = get_integer_value(get_backtrace_entry_opcode(value));
  backtrace_entry_invocation_print_on(invocation, opcode, context);
}

static bool is_invocation_opcode(opcode_t op) {
  switch (op) {
  case ocInvoke:
  case ocSignalEscape:
  case ocSignalContinue:
  case ocBuiltinMaybeEscape:
  case ocCallEnsurer:
    return true;
  default:
    return false;
  }
}

value_t capture_backtrace_entry(runtime_t *runtime, frame_t *frame) {
  // Check whether the program counter stored for this frame points immediately
  // after an invoke instruction. If it does we'll use that instruction to
  // construct the entry.
  value_t code_block = frame_get_code_block(frame);
  value_t bytecode = get_code_block_bytecode(code_block);
  size_t pc = frame->pc;
  if (pc < kInvokeOperationSize)
    return nothing();
  blob_t data;
  get_blob_data(bytecode, &data);
  opcode_t op = (opcode_t) blob_short_at(&data, pc - kInvokeOperationSize);
  if (!is_invocation_opcode(op))
    return nothing();
  value_t tags = whatever();
  if (op == ocCallEnsurer) {
    return new_heap_backtrace_entry(runtime, nothing(), new_integer(op));
  } else if (op == ocBuiltinMaybeEscape) {
    // A builtin escape leaves the record on the stack. Pop it off. This doesn't
    // actually modify the stack only the frame.
    tags = frame_pop_value(frame);
  } else {
    // Okay so we have an invoke we can use. Grab the invocation record which
    // is baked into the instruction.
    size_t record_index = blob_short_at(&data, pc - kInvokeOperationSize + 1);
    value_t value_pool = get_code_block_value_pool(code_block);
    tags = get_array_at(value_pool, record_index);
  }
  // Scan through the record to build the invocation map.
  TRY_DEF(invocation, new_heap_id_hash_map(runtime, 16));
  size_t arg_count = get_call_tags_entry_count(tags);
  for (size_t i = 0;  i < arg_count; i++) {
    value_t tag = get_call_tags_tag_at(tags, i);
    value_t arg = frame_get_pending_argument_at(frame, tags, i);
    TRY(set_id_hash_map_at(runtime, invocation, tag, arg));
  }
  // Wrap the result in a backtrace entry.
  return new_heap_backtrace_entry(runtime, invocation, new_integer(op));
}
