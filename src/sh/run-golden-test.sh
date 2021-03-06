#!/bin/sh
# Copyright 2013 the Neutrino authors (see AUTHORS).
# Licensed under the Apache License, Version 2.0 (see LICENSE).

# Runner script for the golden tests. A golden test is the simplest kind (which
# also means that you only want to write them if there's no other way). Runs a
# program through the runtime and compares the output on stdout with the
# expected ("golden") output given along with the test case.

# Parse command-line arguments.

SRC=$(dirname $(dirname $0))
PLOPT=$SRC/../tools/plopt
TEST_FILE=
EXECUTABLE=

while getopts "t:e:l:" OPT; do
  case "$OPT" in
    t)
      TEST_FILE="$OPTARG"
      ;;
    e)
      EXECUTABLE="$OPTARG"
      ;;
    l)
      LIBRARY="$OPTARG"
      ;;
  esac
done

# Scan through the test file and pick out INPUT/VALUE clauses. This uses a
# state machine-like structure that stores them in variables and when the
# variables have both been set executes the test and starts over.

INPUT=
HAS_INPUT=0
VALUE=
HAS_VALUE=0
OUTPUT=
HAS_OUTPUT=0
HAS_END=0
I=0

# Print a progress marker from 0-9 with a space between blocks of 10.
print_progress() {
  echo -n $I
  I=$((I + 1))
  if [ "$I" -eq "10" ]; then
    I=0
    echo -n " "
  fi
}

# Trims leading and (possibly?) trailing whitespace from a string. This is a bit
# hacky so if there's an issue with whitespace this might be the cause.
trim_space() {
  echo -n "$*" | sed -e "s/^\s*\(.*\)\s*$/\1/g"
}

# Checks that the output from a test was the expected value, otherwise prints
# an error and bails.
check_result() {
  EXPECTED=$(trim_space "$1")
  FOUND="$2"
  INPUT=$(trim_space "$3")
  if [ "$EXPECTED" != "$FOUND" ]; then
    # Test failed. Display an error.
    echo
    printf "Error on input '%s':\\n" "$INPUT"
    printf "  Expected: '%s'\\n" "$EXPECTED"
    printf "  Found: '%s'\\n" "$FOUND"
    COMMAND="$4"
    printf "  Compile: $COMMAND '$INPUT' --base64\\n"
    RUN="$5"
    printf "  Run: $RUN\\n"
    exit 1
  fi
}

# Run an individual golden test.
run_test() {
  # Load all files in src/n as modules.
  print_progress
  COMPILE="$1"
  INPUT="$2"
  OUTPUT="$3"
  RUN="$4"
  FOUND=$($COMPILE "$INPUT" | $RUN - 2>&1 | grep "^[^#]")
  check_result "$OUTPUT" "$FOUND" "$INPUT" "$COMPILE" "$RUN"
}

MAIN_OPTIONS="--main-options `$PLOPT --module_loader { --libraries [ $LIBRARY ] }`"

while read LINE; do
  # Strip end-of-line comments.
  LINE=$(echo "$LINE" | sed -e s/^\\s*#.*$//g)
  if echo "$LINE" | grep '\-\- INPUT:' > /dev/null; then
    # Found an input clause.
    INPUT=$(echo "$LINE" | sed -e "s/^-- INPUT:\\s*//g")
    HAS_INPUT=1
  elif echo "$LINE" | grep '\-\- VALUE:' > /dev/null; then
    # Found a value clause.
    VALUE=$(echo "$LINE" | sed -e "s/^-- VALUE:\\s*//g")
    HAS_VALUE=1
  elif echo "$LINE" | grep '\-\- OUTPUT:' > /dev/null; then
    # Found an output clause.
    OUTPUT=$(echo "$LINE" | sed -e "s/^-- OUTPUT:\\s*//g")
    HAS_OUTPUT=1
  elif echo "$LINE" | grep '\-\- END' > /dev/null; then
    HAS_END=1
  elif [ $HAS_INPUT -eq 1 -a $HAS_OUTPUT -eq 0 ]; then
    # There's already some input but no output clause append to the input.
    INPUT="$INPUT $LINE"
  elif [ $HAS_INPUT -eq 1 -a $HAS_OUTPUT -eq 1 -a $HAS_END -eq 0 ]; then
    # There's already some output but no end clause; append to the output.
    if [ "$OUTPUT" != "" ]; then
      # Separate output lines by newline, except don't add leading newlines.
      OUTPUT="$OUTPUT\n"
    fi
    OUTPUT="$OUTPUT$LINE"
  elif [ -n "$LINE" ]; then
    # Any nonempty line that didn't match above is an error; report it.
    printf "Unexpected line '%s'\\n" "$LINE"
    exit 1
  fi
  if [ $HAS_INPUT -eq 1 -a $HAS_VALUE -eq 1 ]; then
    # If we now have both an INPUT and a VALUE line run the test.
    COMMAND="$SRC/python/neutrino/main.py --expression"
    run_test "$COMMAND" "$INPUT" "$VALUE" "$EXECUTABLE --print-value $MAIN_OPTIONS"
    INPUT=
    HAS_INPUT=0
    VALUE=
    HAS_VALUE=0
  elif [ $HAS_INPUT -eq 1 -a $HAS_OUTPUT -eq 1 -a $HAS_END -eq 1 ]; then
    # If we now have both an INPUT and an OUTPUT line run the test.
    COMMAND="$SRC/python/neutrino/main.py --program"
    run_test "$COMMAND" "$INPUT" "$OUTPUT" "$EXECUTABLE $MAIN_OPTIONS"
    INPUT=
    HAS_INPUT=0
    OUTPUT=
    HAS_OUTPUT=0
    HAS_END=0
  fi
done < "$TEST_FILE"

# We completed successfully so we can signal success by touching the output
# file (as well as implicitly exiting 0).

if [ $HAS_INPUT -eq 1 -o $HAS_VALUE -eq 1 -o $HAS_OUTPUT -eq 1 -o -$HAS_END -eq 1 ]; then
  echo
  echo "Incomplete test"
  exit 1
fi

echo
