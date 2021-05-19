#!/usr/bin/env python
# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
A Deterministic acyclic finite state automaton (DAFSA) is a compact
representation of an unordered word list (dictionary).

http://en.wikipedia.org/wiki/Deterministic_acyclic_finite_state_automaton

This python program converts a list of strings to a byte array in C++.
This python program fetches strings and return values from a gperf file
and generates a C++ file with a byte array representing graph that can be
used as a memory efficient replacement for the perfect hash table.

The input strings are assumed to consist of printable 7-bit ASCII characters
and the return values are assumed to be one digit integers.

In this program a DAFSA is a diamond shaped graph starting at a common
root node and ending at a common end node. All internal nodes contain
a character and each word is represented by the characters in one path from
the root node to the end node.

The order of the operations is crucial since lookups will be performed
starting from the source with no backtracking. Thus a node must have at
most one child with a label starting by the same character. The output
is also arranged so that all jumps are to increasing addresses, thus forward
in memory.

The generated output has suffix free decoding so that the sign of leading
bits in a link (a reference to a child node) indicate if it has a size of one,
two or three bytes and if it is the last outgoing link from the actual node.
A node label is terminated by a byte with the leading bit set.

The generated byte array can described by the following BNF:

<byte> ::= < 8-bit value in range [0x00-0xFF] >

<char> ::= < printable 7-bit ASCII character, byte in range [0x20-0x7F] >
<end_char> ::= < char + 0x80, byte in range [0xA0-0xFF] >
<return value> ::= < value + 0x80, byte in range [0x80-0x8F] >

<offset1> ::= < byte in range [0x00-0x3F] >
<offset2> ::= < byte in range [0x40-0x5F] >
<offset3> ::= < byte in range [0x60-0x7F] >

<end_offset1> ::= < byte in range [0x80-0xBF] >
<end_offset2> ::= < byte in range [0xC0-0xDF] >
<end_offset3> ::= < byte in range [0xE0-0xFF] >

<prefix> ::= <char>

<label> ::= <end_char>
          | <char> <label>

<end_label> ::= <return_value>
          | <char> <end_label>

<offset> ::= <offset1>
           | <offset2> <byte>
           | <offset3> <byte> <byte>

<end_offset> ::= <end_offset1>
               | <end_offset2> <byte>
               | <end_offset3> <byte> <byte>

<offsets> ::= <end_offset>
            | <offset> <offsets>

<source> ::= <offsets>

<node> ::= <label> <offsets>
         | <prefix> <node>
         | <end_label>

<dafsa> ::= <source>
          | <dafsa> <node>

Decoding:

<char> -> printable 7-bit ASCII character
<end_char> & 0x7F -> printable 7-bit ASCII character
<return value> & 0x0F -> integer
<offset1 & 0x3F> -> integer
((<offset2> & 0x1F>) << 8) + <byte> -> integer
((<offset3> & 0x1F>) << 16) + (<byte> << 8) + <byte> -> integer

end_offset1, end_offset2 and and_offset3 are decoded same as offset1,
offset2 and offset3 respectively.

The first offset in a list of offsets is the distance in bytes between the
offset itself and the first child node. Subsequent offsets are the distance
between previous child node and next child node. Thus each offset links a node
to a child node. The distance is always counted between start addresses, i.e.
first byte in decoded offset or first byte in child node.

Example 1:

%%
aa, 1
a, 2
%%

The input is first parsed to a list of words:
["aa1", "a2"]

This produces the following graph:
[root] --- a --- 0x02 --- [end]
            |              /
             |           /
              - a --- 0x01

A C++ representation of the compressed graph is generated:

const unsigned char dafsa[7] = {
  0x81, 0xE1, 0x02, 0x81, 0x82, 0x61, 0x81,
};

The bytes in the generated array has the following meaning:

 0: 0x81 <end_offset1>  child at position 0 + (0x81 & 0x3F) -> jump to 1

 1: 0xE1 <end_char>     label character (0xE1 & 0x7F) -> match "a"
 2: 0x02 <offset1>      child at position 2 + (0x02 & 0x3F) -> jump to 4

 3: 0x81 <end_offset1>  child at position 4 + (0x81 & 0x3F) -> jump to 5
 4: 0x82 <return_value> 0x82 & 0x0F -> return 2

 5: 0x61 <char>         label character 0x61 -> match "a"
 6: 0x81 <return_value> 0x81 & 0x0F -> return 1

Example 2:

%%
aa, 1
bbb, 2
baa, 1
%%

The input is first parsed to a list of words:
["aa1", "bbb2", "baa1"]

This produces the following graph:
[root] --- a --- a --- 0x01 --- [end]
 |       /           /         /
  |    /           /         /
   - b --- b --- b --- 0x02

A C++ representation of the compressed graph is generated:

const unsigned char dafsa[11] = {
  0x02, 0x83, 0xE2, 0x02, 0x83, 0x61, 0x61, 0x81, 0x62, 0x62, 0x82,
};

The bytes in the generated array has the following meaning:

 0: 0x02 <offset1>      child at position 0 + (0x02 & 0x3F) -> jump to 2
 1: 0x83 <end_offset1>  child at position 2 + (0x83 & 0x3F) -> jump to 5

 2: 0xE2 <end_char>     label character (0xE2 & 0x7F) -> match "b"
 3: 0x02 <offset1>      child at position 3 + (0x02 & 0x3F) -> jump to 5
 4: 0x83 <end_offset1>  child at position 5 + (0x83 & 0x3F) -> jump to 8

 5: 0x61 <char>         label character 0x61 -> match "a"
 6: 0x61 <char>         label character 0x61 -> match "a"
 7: 0x81 <return_value> 0x81 & 0x0F -> return 1

 8: 0x62 <char>         label character 0x62 -> match "b"
 9: 0x62 <char>         label character 0x62 -> match "b"
10: 0x82 <return_value> 0x82 & 0x0F -> return 2
"""
import sys
import struct

from incremental_dafsa import Dafsa, Node


class InputError(Exception):
    """Exception raised for errors in the input file."""


def top_sort(dafsa: Dafsa):
    """Generates list of nodes in topological sort order."""
    incoming = {}

    def count_incoming(node: Node):
        """Counts incoming references."""
        if not node.is_end_node:
            if id(node) not in incoming:
                incoming[id(node)] = 1
                for child in node.children.values():
                    count_incoming(child)
            else:
                incoming[id(node)] += 1

    for node in dafsa.root_node.children.values():
        count_incoming(node)

    for node in dafsa.root_node.children.values():
        incoming[id(node)] -= 1

    waiting = [
        node for node in dafsa.root_node.children.values() if incoming[id(node)] == 0
    ]
    nodes = []

    while waiting:
        node = waiting.pop()
        assert incoming[id(node)] == 0
        nodes.append(node)
        for child in node.children.values():
            if not child.is_end_node:
                incoming[id(child)] -= 1
                if incoming[id(child)] == 0:
                    waiting.append(child)
    return nodes


def encode_links(node: Node, offsets, current):
    """Encodes a list of children as one, two or three byte offsets."""
    if next(iter(node.children.values())).is_end_node:
        # This is an <end_label> node and no links follow such nodes
        return []
    guess = 3 * len(node.children)
    assert node.children

    children = sorted(node.children.values(), key=lambda x: -offsets[id(x)])
    while True:
        offset = current + guess
        buf = []
        for child in children:
            last = len(buf)
            distance = offset - offsets[id(child)]
            assert distance > 0 and distance < (1 << 21)

            if distance < (1 << 6):
                # A 6-bit offset: "s0xxxxxx"
                buf.append(distance)
            elif distance < (1 << 13):
                # A 13-bit offset: "s10xxxxxxxxxxxxx"
                buf.append(0x40 | (distance >> 8))
                buf.append(distance & 0xFF)
            else:
                # A 21-bit offset: "s11xxxxxxxxxxxxxxxxxxxxx"
                buf.append(0x60 | (distance >> 16))
                buf.append((distance >> 8) & 0xFF)
                buf.append(distance & 0xFF)
            # Distance in first link is relative to following record.
            # Distance in other links are relative to previous link.
            offset -= distance
        if len(buf) == guess:
            break
        guess = len(buf)
    # Set most significant bit to mark end of links in this node.
    buf[last] |= 1 << 7
    buf.reverse()
    return buf


def encode_prefix(label):
    """Encodes a node label as a list of bytes without a trailing high byte.

    This method encodes a node if there is exactly one child  and the
    child follows immediately after so that no jump is needed. This label
    will then be a prefix to the label in the child node.
    """
    assert label
    return [ord(c) for c in reversed(label)]


def encode_label(label):
    """Encodes a node label as a list of bytes with a trailing high byte >0x80."""
    buf = encode_prefix(label)
    # Set most significant bit to mark end of label in this node.
    buf[0] |= 1 << 7
    return buf


def encode(dafsa: Dafsa):
    """Encodes a DAFSA to a list of bytes"""
    output = []
    offsets = {}

    for node in reversed(top_sort(dafsa)):
        if (
            len(node.children) == 1
            and not next(iter(node.children.values())).is_end_node
            and (offsets[id(next(iter(node.children.values())))] == len(output))
        ):
            output.extend(encode_prefix(node.character))
        else:
            output.extend(encode_links(node, offsets, len(output)))
            output.extend(encode_label(node.character))
        offsets[id(node)] = len(output)

    output.extend(encode_links(dafsa.root_node, offsets, len(output)))
    output.reverse()
    return output


def to_cxx(data, preamble=None):
    """Generates C++ code from a list of encoded bytes."""
    text = "/* This file is generated. DO NOT EDIT!\n\n"
    text += "The byte array encodes a dictionary of strings and values. See "
    text += "make_dafsa.py for documentation."
    text += "*/\n\n"

    if preamble:
        text += preamble
        text += "\n\n"

    text += "const unsigned char kDafsa[%s] = {\n" % len(data)
    for i in range(0, len(data), 12):
        text += "  "
        text += ", ".join("0x%02x" % byte for byte in data[i : i + 12])
        text += ",\n"
    text += "};\n"
    return text


def words_to_cxx(words, preamble=None):
    """Generates C++ code from a word list"""
    dafsa = Dafsa.from_tld_data(words)
    return to_cxx(encode(dafsa), preamble)


def words_to_bin(words):
    """Generates bytes from a word list"""
    dafsa = Dafsa.from_tld_data(words)
    data = encode(dafsa)
    return struct.pack("%dB" % len(data), *data)


def parse_gperf(infile):
    """Parses gperf file and extract strings and return code"""
    lines = [line.strip() for line in infile]

    # Extract the preamble.
    first_delimeter = lines.index("%%")
    preamble = "\n".join(lines[0:first_delimeter])

    # Extract strings after the first '%%' and before the second '%%'.
    begin = first_delimeter + 1
    end = lines.index("%%", begin)
    lines = lines[begin:end]
    for line in lines:
        if line[-3:-1] != ", ":
            raise InputError('Expected "domainname, <digit>", found "%s"' % line)
        # Technically the DAFSA format could support return values in range [0-31],
        # but the values below are the only with a defined meaning.
        if line[-1] not in "0124":
            raise InputError(
                'Expected value to be one of {0,1,2,4}, found "%s"' % line[-1]
            )
    return (preamble, [line[:-3] + line[-1] for line in lines])


def main(outfile, infile):
    with open(infile, "r") as infile:
        preamble, words = parse_gperf(infile)
        outfile.write(words_to_cxx(words, preamble))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: %s infile outfile" % sys.argv[0])
        sys.exit(1)

    with open(sys.argv[2], "w") as outfile:
        sys.exit(main(outfile, sys.argv[1]))