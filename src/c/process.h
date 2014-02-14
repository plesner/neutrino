// Copyright 2013 the Neutrino authors (see AUTHORS).
// Licensed under the Apache License, Version 2.0 (see LICENSE).

// Objects and functionality related to processes and execution.


#ifndef _PROCESS
#define _PROCESS

#include "value-inl.h"

// --- S t a c k   p i e c e ---

#define CHECK_STACK_PIECE_DISCIPLINE true

static const size_t kStackPieceSize = OBJECT_SIZE(7);
static const size_t kStackPieceStorageOffset = OBJECT_FIELD_OFFSET(0);
static const size_t kStackPiecePreviousOffset = OBJECT_FIELD_OFFSET(1);
static const size_t kStackPieceTopFramePointerOffset = OBJECT_FIELD_OFFSET(2);
static const size_t kStackPieceTopStackPointerOffset = OBJECT_FIELD_OFFSET(3);
static const size_t kStackPieceTopCapacityOffset = OBJECT_FIELD_OFFSET(4);
static const size_t kStackPieceTopFlagsOffset = OBJECT_FIELD_OFFSET(5);
static const size_t kStackPieceIsClosedOffset = OBJECT_FIELD_OFFSET(6);

// The plain array used for storage for this stack piece.
ACCESSORS_DECL(stack_piece, storage);

// The previous, lower, stack piece.
ACCESSORS_DECL(stack_piece, previous);

// The frame pointer for the top frame.
INTEGER_ACCESSORS_DECL(stack_piece, top_frame_pointer);

// The current stack pointer for the top frame.
INTEGER_ACCESSORS_DECL(stack_piece, top_stack_pointer);

// The capacity for the top frame.
INTEGER_ACCESSORS_DECL(stack_piece, top_capacity);

// A tiny bit set that describes the top frame.
ACCESSORS_DECL(stack_piece, top_flags);

// Is this stack piece closed such that it must be opened before execution on it
// can continue?
ACCESSORS_DECL(stack_piece, is_closed);

// Flags that describe a stack frame.
typedef enum {
  // This is a maintenance frame inserted by the runtime.
  ffSynthetic        = 0x01,
  // This is a bottom frame of a stack piece, the one that returns to the
  // previous stack piece.
  ffStackPieceBottom = 0x02,
  // This isn't a real frame but the initial state of a stack piece that has
  // no frames.
  ffStackPieceEmpty  = 0x04,
  // This is the bottom frame of a stack, the one that ends execution on that
  // stack.
  ffStackBottom      = 0x08,
  // This is an organic stack frame generated by a method invocation.
  ffOrganic          = 0x10
} frame_flag_t;


// --- F r a m e ---

// A transient stack frame. The current structure isn't clever but it's not
// intended to be, it's intended to work and be fully general.
typedef struct {
  // Pointer to the next available field on the stack.
  size_t stack_pointer;
  // Pointer to the bottom of the stack fields.
  size_t frame_pointer;
  // The total frame capacity available.
  size_t capacity;
  // The flags describing this frame.
  value_t flags;
  // The stack piece that contains this frame.
  value_t stack_piece;
} frame_t;

// The number of word-size fields in a frame.
#define kFrameFieldCount 5

// The number of words in a stack frame header.
static const size_t kFrameHeaderSize
    = (kFrameFieldCount - 2)  // The frame fields minus the stack pointer which
                              //   is implicit and the stack piece.
    + 1                       // The code block
    + 1                       // The PC
    + 1;                      // The argument map

// Offsets _down_ from the frame pointer to the header fields.
static const size_t kFrameHeaderPreviousFramePointerOffset = 0;
static const size_t kFrameHeaderPreviousCapacityOffset = 1;
static const size_t kFrameHeaderPreviousFlagsOffset = 2;
static const size_t kFrameHeaderCodeBlockOffset = 3;
static const size_t kFrameHeaderPcOffset = 4;
static const size_t kFrameHeaderArgumentMapOffset = 5;

// Tries to allocate a new frame on the given stack piece of the given capacity.
// Returns true iff allocation succeeds.
bool try_push_stack_piece_frame(value_t stack_piece, frame_t *frame,
    size_t capacity, uint32_t flags);

// Puts the given stack piece in the open state and stores the state required to
// interact with it in the given frame.
void open_stack_piece(value_t stack_piece, frame_t *frame);

// Records the state stored in the given frame in its stack piece and closes
// the stack piece.
void close_stack_piece(frame_t *frame);

// Pops the top frame off the given stack piece, storing the next frame in the
// given frame struct. Returns true if there are more frames to pop off the stack,
// false if the one popped off was the last one. If false is returned the frame
// is invalid.
void pop_stack_piece_frame(value_t stack_piece, frame_t *frame);

// Record the frame pointer for the previous stack frame, the one below this one.
void set_frame_previous_frame_pointer(frame_t *frame, size_t value);

// Returns the frame pointer for the previous stack frame, the one below this
// one.
size_t get_frame_previous_frame_pointer(frame_t *frame);

// Record the capacity of the previous stack frame.
void set_frame_previous_capacity(frame_t *frame, size_t capacity);

// Returns the capacity of the previous stack frame.
size_t get_frame_previous_capacity(frame_t *frame);

// Record the flags of the previous stack frame.
void set_frame_previous_flags(frame_t *frame, value_t flags);

// Returns the flags of the previous stack frame.
value_t get_frame_previous_flags(frame_t *frame);

// Sets the code block this frame is executing.
void set_frame_code_block(frame_t *frame, value_t code_block);

// Returns the code block this frame is executing.
value_t get_frame_code_block(frame_t *frame);

// Sets the program counter for this frame.
void set_frame_pc(frame_t *frame, size_t pc);

// Returns the program counter for this frame.
size_t get_frame_pc(frame_t *frame);

// Sets the mapping from parameters to argument indices for this frame.
void set_frame_argument_map(frame_t *frame, value_t map);

// Returns the mapping from parameter to argument indices for this frame.
value_t get_frame_argument_map(frame_t *frame);

// Pushes a value onto this stack frame. The returned value will always be
// success except on bounds check failures in soft check failure mode where it
// will be OutOfBounds.
value_t frame_push_value(frame_t *frame, value_t value);

// Pops a value off this stack frame. Bounds checks whether there is a value to
// pop and in soft check failure mode returns an OutOfBounds condition if not.
value_t frame_pop_value(frame_t *frame);

// Returns the index'th value counting from the top of this stack. Bounds checks
// whether there is a value to return and in soft check failure mode returns an
// OutOfBounds condition if not.
value_t frame_peek_value(frame_t *frame, size_t index);

// Returns the value of the index'th parameter.
value_t frame_get_argument(frame_t *frame, size_t param_index);

// Returns the value of the index'th local variable in this frame.
value_t frame_get_local(frame_t *frame, size_t index);

// Is this frame synthetic, that is, does it correspond to an activation
// inserted by the runtime and not caused by an invocation?
bool frame_has_flag(frame_t *frame, frame_flag_t flag);


// --- F r a m e   i t e r a t o r ---

// Data used while iterating the frames of a stack.
typedef struct {
  // The currently active frame.
  frame_t current;
} frame_iter_t;

// Initializes the given frame iterator. After this call the current frame will
// be the top frame of the stack.
void frame_iter_init_from_stack(frame_iter_t *iter, value_t stack);

// Initializes the given frame iterator. After this call the current frame will
// be the one passed as an argument.
void frame_iter_init_from_frame(frame_iter_t *iter, frame_t *frame);

// Returns the current frame. The result is well-defined until the first call to
// frame_iter_advance that returns false.
frame_t *frame_iter_get_current(frame_iter_t *iter);

// Advances the iterator to the next frame. Returns true iff advancing was
// successful, in which case frame_iter_get_current can be called to get the
// next frame.
bool frame_iter_advance(frame_iter_t *iter);


// --- S t a c k ---

static const size_t kStackSize = OBJECT_SIZE(2);
static const size_t kStackTopPieceOffset = OBJECT_FIELD_OFFSET(0);
static const size_t kStackDefaultPieceCapacityOffset = OBJECT_FIELD_OFFSET(1);

// The top stack piece of this stack.
ACCESSORS_DECL(stack, top_piece);

// The default capacity of the stack pieces that make up this stack.
INTEGER_ACCESSORS_DECL(stack, default_piece_capacity);

// Allocates a new frame on this stack. If allocating fails, for instance if a
// new stack piece is required and we're out of memory, a condition is returned.
// The arg map array is used to determine how many arguments should be copied
// from the old to the new segment in the case where we have to create a new
// one. It is passed as a value rather than a size because the value is easily
// available wherever this gets called and it saves reading the size in the
// common case where no new segment gets allocated. If you're _absolutely_ sure
// no new segment will be allocated you can pass null for the arg map.
value_t push_stack_frame(runtime_t *runtime, value_t stack, frame_t *frame,
    size_t frame_capacity, value_t arg_map);

// Pops frames off the given stack until one is reached that has one of the
// given flags set. There must be such a frame on the stack.
void drop_to_stack_frame(value_t stack, frame_t *frame, frame_flag_t flags);

// Reads the top frame off the given stack into the given frame.
void get_stack_top_frame(value_t stack, frame_t *frame);


// --- E s c a p e ---

static const size_t kEscapeSize = OBJECT_SIZE(3);
static const size_t kEscapeIsLiveOffset = OBJECT_FIELD_OFFSET(0);
static const size_t kEscapeStackPieceOffset = OBJECT_FIELD_OFFSET(1);
static const size_t kEscapeStackPointerOffset = OBJECT_FIELD_OFFSET(2);

// The number of stack entries it takes to record the complete state of a frame.
static const size_t kCapturedStateSize
    = (kFrameFieldCount - 1) // Everything from the frame except the stack piece.
    + 1;                     // The PC.

// Is it valid to invoke this escape, that is, are we still within the body of
// the with_escape block that produced this escape?
ACCESSORS_DECL(escape, is_live);

// The stack piece to drop to when escaping.
ACCESSORS_DECL(escape, stack_piece);

// The stack pointer that indicates where the stored state is located on the
// stack piece.
ACCESSORS_DECL(escape, stack_pointer);


// --- B a c k t r a c e ---

static const size_t kBacktraceSize = OBJECT_SIZE(1);
static const size_t kBacktraceEntriesOffset = OBJECT_FIELD_OFFSET(0);

// The array buffer of backtrace entries.
ACCESSORS_DECL(backtrace, entries);

// Creates a new backtrace by traversing the stack starting from the given
// frame.
value_t capture_backtrace(runtime_t *runtime, frame_t *frame);


// --- B a c k t r a c e   e n t r y ---

static const size_t kBacktraceEntrySize = OBJECT_SIZE(2);
static const size_t kBacktraceEntryInvocationOffset = OBJECT_FIELD_OFFSET(0);
static const size_t kBacktraceEntryIsSignalOffset = OBJECT_FIELD_OFFSET(1);

// The invocation record for this entry.
ACCESSORS_DECL(backtrace_entry, invocation);

// Is this backtrace entry the result of a signal being raised or a normal call?
ACCESSORS_DECL(backtrace_entry, is_signal);

// Print the given invocation map on the given context. This is really an
// implementation detail of how backtrace entries print themselves but it's
// tricky enough that it makes sense to be able to test as a separate thing.
void backtrace_entry_invocation_print_on(value_t invocation, bool is_signal,
    print_on_context_t *context);

// Creates a backtrace entry from the given stack frame. If no entry can be
// created nothing is returned.
value_t capture_backtrace_entry(runtime_t *runtime, frame_t *frame);


#endif // _PROCESS
