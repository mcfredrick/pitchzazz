#include <DSP/MidiNoteStack.h>
#include <catch2/catch_test_macros.hpp>

using namespace pitchzazz;

TEST_CASE ("empty stack has no active note", "[midi-note-stack]")
{
    MidiNoteStack stack;
    CHECK (stack.activeNote() == -1);
}

TEST_CASE ("a single held note becomes active", "[midi-note-stack]")
{
    MidiNoteStack stack;
    stack.noteOn (60);
    CHECK (stack.activeNote() == 60);
}

TEST_CASE ("most-recently-pressed note wins", "[midi-note-stack]")
{
    MidiNoteStack stack;
    stack.noteOn (60);
    stack.noteOn (64);
    stack.noteOn (67);
    CHECK (stack.activeNote() == 67);
}

TEST_CASE ("releasing the active note falls back to the next most recent, not the oldest", "[midi-note-stack]")
{
    MidiNoteStack stack;
    stack.noteOn (60);
    stack.noteOn (64);
    stack.noteOn (67);
    stack.noteOff (67);
    CHECK (stack.activeNote() == 64);
}

TEST_CASE ("releasing a non-active held note doesn't disturb the active one", "[midi-note-stack]")
{
    MidiNoteStack stack;
    stack.noteOn (60);
    stack.noteOn (64);
    stack.noteOn (67); // active

    stack.noteOff (60); // release the *oldest*, not the active note
    CHECK (stack.activeNote() == 67);

    stack.noteOff (64); // release a middle note
    CHECK (stack.activeNote() == 67); // still unaffected
}

TEST_CASE ("releasing every held note returns to no active note", "[midi-note-stack]")
{
    MidiNoteStack stack;
    stack.noteOn (60);
    stack.noteOn (64);
    stack.noteOff (64);
    stack.noteOff (60);
    CHECK (stack.activeNote() == -1);
}

TEST_CASE ("a duplicate note-on doesn't change priority order", "[midi-note-stack]")
{
    MidiNoteStack stack;
    stack.noteOn (60);
    stack.noteOn (64);
    stack.noteOn (60); // already held -- should be a no-op, not re-promote 60 to active
    CHECK (stack.activeNote() == 64);
}

TEST_CASE ("out-of-range note numbers are ignored, not undefined behaviour", "[midi-note-stack]")
{
    MidiNoteStack stack;
    stack.noteOn (-1);
    stack.noteOn (128);
    CHECK (stack.activeNote() == -1);

    stack.noteOff (-1); // must not crash/UB on an unheld, out-of-range note
    stack.noteOff (200);
    CHECK (stack.activeNote() == -1);
}
