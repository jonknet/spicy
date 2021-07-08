
.. _toolchain:

=========
Toolchain
=========

.. _spicy-build:

``spicy-build``
===============

``spicy-build`` is a shell frontend that compiles Spicy source code
into a standalone executable by running :ref:`spicyc` to generate the
necessary C++ code, then spawning the system compiler to compile and
link that.

.. spicy-output:: usage-spicy-build
    :exec: spicy-build -h

.. _spicy-config:

``spicy-config``
================

``spicy-config`` reports information about Spicy's build &
installation options.

.. spicy-output:: usage-spicy-config
    :exec: spicy-config -h

.. _spicyc:

``spicyc``
==========

``spicyc`` compiles Spicy code into C++ output, optionally also
executing it directly through JIT.

.. spicy-output:: usage-spicyc
    :exec: spicyc -h

``spicyc`` also supports the following environment variables to
control the compilation process:

	``SPICY_PATH``
        Replaces the built-in search path for `*.spicy` source files.

    ``SPICY_CACHE``
        Location for storing precompiled C++ headers. Default is ``~/.cache/spicy/<VERSION>``.

    ``HILTI_CXX``
        Specifies the path to the C++ compiler to use.

    ``HILTI_CXX_INCLUDE_DIRS``
        Specified additional, colon-separated C++ include directory to
        search for header files.

    ``HILTI_JIT_SEQUENTIAL``
        Set to prevent spawning multiple concurrent C++ compiler instances.

    ``HILTI_PATH``
        Replaces the built-in search path for `*.hlt` source files.

    ``HILTI_PRINT_SETTINGS``
        Set to see summary of compilation options.

.. _spicy-driver:

``spicy-driver``
================

``spicy-driver`` is a standalone Spicy host application that compiles
and executes Spicy parsers on the fly, and then feeds them data for
parsing from standard input.

.. spicy-output:: usage-spicy-driver
    :exec: spicy-driver -h

``spicy-driver`` supports the same environment variables as
:ref:`spicyc`.

Specifying the parser to use
----------------------------

If there's only single ``public`` unit in the Spicy source code,
``spicy-driver`` will automatically use that for parsing its input. If
there's more than one public unit, you need to tell ``spicy-driver``
which one to use through its ``--parser`` (or ``-p``) option. To see
the parsers that are available, use ``--list-parsers`` (or ``-l``).

In addition to the names shown by ``--list-parsers``, you can also
specify a parser through a port or MIME type if the corresponding unit
:ref:`defines them through properties <unit_meta_data>`. For example,
if a unit defines ``%port = 80/tcp``, you can use ``spicy-driver -p
80/tcp`` to select it. To specify a direction, add either ``%orig`` or
``%resp`` (e.g., ``-p 80/tcp%resp``); then only units with a port
tagged with an ``&originator`` or ``&responder`` attribute,
respectively, will be considered. If a unit defines ``%mime-type =
application/test``, you can select it through ``spicy-driver -p
application/test``. (Note that there must be exactly one unit with a
matching property for this all to work, otherwise you'll get an error
message.)

Batch input
-----------

``spicy-driver`` provides a batch input mode for processing multiple
interleaved input flows in parallel, mimicking how host applications
like Zeek would be employing Spicy parsers for processing many
sessions concurrently. The batch input must be prepared in a specific
format (see below) that provides embedded meta information about the
contained flows of input. The easiest way to generate such a batch
is :download:`a Zeek script coming with Spicy
</_static/record-spicy-batch.zeek>`. If you run Zeek with this script
on a PCAP trace, it will record the contained TCP and UDP sessions
into a Spicy batch file::

    # zeek -b -r http/methods.trace record-spicy-batch.zeek
    tracking [orig_h=128.2.6.136, orig_p=46562/tcp, resp_h=173.194.75.103, resp_p=80/tcp]
    tracking [orig_h=128.2.6.136, orig_p=46563/tcp, resp_h=173.194.75.103, resp_p=80/tcp]
    tracking [orig_h=128.2.6.136, orig_p=46564/tcp, resp_h=173.194.75.103, resp_p=80/tcp]
    tracking [orig_h=128.2.6.136, orig_p=46565/tcp, resp_h=173.194.75.103, resp_p=80/tcp]
    tracking [orig_h=128.2.6.136, orig_p=46566/tcp, resp_h=173.194.75.103, resp_p=80/tcp]
    tracking [orig_h=128.2.6.136, orig_p=46567/tcp, resp_h=173.194.75.103, resp_p=80/tcp]
    [...]
    tracking [orig_h=128.2.6.136, orig_p=46608/tcp, resp_h=173.194.75.103, resp_p=80/tcp]
    tracking [orig_h=128.2.6.136, orig_p=46609/tcp, resp_h=173.194.75.103, resp_p=80/tcp]
    tracking [orig_h=128.2.6.136, orig_p=46610/tcp, resp_h=173.194.75.103, resp_p=80/tcp]
    recorded 49 sessions total
    output in batch.dat

You will now have a file ``batch.dat`` that you can use with
``spicy-driver -F batch.data ...``.

The batch created by the Zeek script will select parsers for the
contained sessions through well-known ports. That means your units
need to have a ``%port`` property matching the responder port of the
sessions you want them to parse. So for the HTTP trace above, our
Spicy source code would need to provide a public unit with property
``%port = 80/tcp;``.

In case you want to create batches yourself, we document the batch
format in the following. A batch needs to start with a line
``!spicy-batch v2<NL>``, followed by lines with commands of the form
``@<tag> <arguments><NL>``.

There are two types of input that the batch format can represent: (1)
individual, uni-directional flows; and (2) bi-directional connections
consisting in turn of one flow per side. The type is determined
through an initial command: ``@begin-flow`` starts a flow flow, and
``@begin-conn`` starts a connection. Either form introduces a unique,
free-form ID that subsequent commands will then refer to. The
following commands are supported:

``@begin-flow FID TYPE PARSER<NL>``
    Initializes a new input flow for parsing, associating the unique
    ID ``FID`` with it. ``TYPE`` must be either ``stream`` for
    stream-based parsing (think: TCP), or ``block`` for parsing each
    data block independent of others (think: UDP). ``PARSER`` is the
    name of the Spicy parser to use for parsing this input flow,
    given in the same form as with ``spicy-driver``'s ``--parser``
    option (i.e., either as a unit name, a ``%port``, or a
    ``%mime-type``).

``@begin-conn CID TYPE ORIG_FID ORIG_PARSER RESP_FID RESP_PARSER<NL>``
    Initializes a new input connection for parsing, associating the
    unique connection ID ``CID`` with it. ``TYPE`` must be either
    ``stream`` for stream-based parsing (think: TCP), or ``block`` for
    parsing each data block independent of others (think: UDP).
    ``ORIG_FID`` is separate unique ID for the originator-side flow,
    and ``ORIG_PARSER`` is the name of the Spicy parser to use for
    parsing that flow. ``RESP_FID`` and ``RESP_PARSER`` work
    accordingly for the responder-side flow. The parsers can be given
    in the same form as with ``spicy-driver``'s ``--parser`` option
    (i.e., either as a unit name, a ``%port``, or a ``%mime-type``).

``@data FID SIZE<NL>``
    A block of data for the input flow ``FID``. This command must be
    followed directly by binary data of length ``SIZE``, plus a final
    newline character. The data represents the next chunk of input for
    the corresponding flow. ``@data`` can be used only inside
    corresponding ``@begin-*`` and ``@end-*`` commands bracketing the
    flow ID.

``@end-flow FID<NL>``
    Finalizes parsing of the input flow associated with ``FID``,
    releasing all state. This must come only after a corresponding
    ``@begin-flow`` command, and every ``@begin-flow`` must eventually
    be followed by an ``@end-flow``.

``@end-conn CID<NL>``
    Finalizes parsing the input connection associated with ``CID``,
    releasing all state (including for its two flows). This must come
    only after a corresponding ``@begin-conn`` command, and every
    ``@begin-conn`` must eventually be followed by an ``@end-end``.

.. _spicy-dump:

``spicy-dump``
==============

``spicy-dump`` is a standalone Spicy host application that compiles
and executes Spicy parsers on the fly, feeds them data for proessing,
and then at the end prints out the parsed information in either a
readable, custom ASCII format, or as JSON (``--json`` or ``-J``). By
default, ``spicy-dump`` disables showing the output of Spicy ``print``
statements, ``--enable-print`` or ``-P`` reenables that.

.. spicy-output:: usage-spicy-dump
    :exec: spicy-dump -h
