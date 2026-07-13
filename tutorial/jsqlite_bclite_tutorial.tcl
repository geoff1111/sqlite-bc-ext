#!/usr/bin/env jimsh

set automatic 0
if {[llength $argv] && [lindex $argv 0] eq "--self-test"} {
    set automatic 1
    set argv [lrange $argv 1 end]
}
set here [file dirname [file normalize [info script]]]
set root [file normalize [file join $here ..]]
set jsqlite [file normalize [file join $root .. jsqlite jsqlite.so]]
set extension [file normalize [file join $root build scbclite.so]]
if {[llength $argv] > 0} { set jsqlite [file normalize [lindex $argv 0]] }
if {[llength $argv] > 1} { set extension [file normalize [lindex $argv 1]] }

proc pause {} {
    global automatic
    if {$automatic} return
    puts -nonewline "\nPress Enter for the next example..."
    flush stdout
    gets stdin
}
proc example {number title code} {
    puts "\n================================================================"
    puts "Example $number: $title"
    puts "================================================================"
    puts "Tcl code:"
    puts [string trim $code]
    puts "\nOutput:"
    uplevel 1 $code
    pause
}
proc query {db sql args} {
    puts "SQL> $sql"
    set result [$db query $sql {*}$args]
    puts $result
    return $result
}
proc expected_error {script} {
    if {[catch {uplevel 1 $script} message]} {
        puts "Expected error: $message"
    } else {
        error "an expected error did not occur"
    }
}

puts "BC-Lite SQLite extension tutorial"
puts "jsqlite module: $jsqlite"
puts "SQLite extension: $extension"
load $jsqlite
set db [jsqlite.open :memory: -extension $extension]

example 1 "Check the extension and its numeric text prefix" {
query $db {select sc_bclite_prefix() as numeric_prefix}
}

example 2 "Load BC-Lite procedures and register public procedures" {
set scalarSource {
proc twice {x} { return [mul $x #2] }
proc square {x} { return [mul $x $x] }
proc cube {x} { return [mul [mul $x $x] $x] }
proc rounded_even {x places} { return [round_half_even $x $places] }
}
puts "BC-Lite source passed from Tcl:"
puts [string trim $scalarSource]
query $db {select sc_bclite_load(?)} $scalarSource
query $db {select sc_bclite_register_all()}
}

example 3 "Call scalar functions from SQL" {
query $db {select twice(21), square(12), cube(3)}
}

example 4 "Observe exact integer and decimal result types" {
query $db {select typeof(twice(21)), twice(21)}
query $db {select typeof(twice('#1.25')), twice('#1.25')}
query $db {select typeof(square('#12345678901234567890')), square('#12345678901234567890')}
}

example 5 "Use embedded math-library procedures inside user procedures" {
query $db {select rounded_even('#2.345', 2), rounded_even('#2.355', 2)}
}

example 6 "Register a procedure under a chosen SQL name" {
query $db {select sc_bclite_register_scalar('bc_double', 'twice', 1)}
query $db {select bc_double('#9.75')}
}

example 7 "Create aggregate callbacks with persistent numeric state" {
set aggregateSource {
proc sum_step {x} {
    state_set #0 [add [state_get #0] $x]
    return [state_get #0]
}
proc count_step {x} {
    state_set #0 [add [state_get #0] $x]
    state_set #1 [add [state_get #1] #1]
    return [state_get #0]
}
proc sum_final {} { return [state_get #0] }
proc average_final {} {
    set old [get_scale]
    set_scale #6
    set result [div [state_get #0] [state_get #1]]
    set_scale $old
    return $result
}
proc sum_inverse {x} {
    state_set #0 [sub [state_get #0] $x]
    return [state_get #0]
}
}
puts "BC-Lite aggregate source:"
puts [string trim $aggregateSource]
query $db {select sc_bclite_load(?)} $aggregateSource
query $db {select sc_bclite_register_aggregate('bc_sum', 1, 1, 'sum_step', 'sum_final')}
query $db {select sc_bclite_register_aggregate('bc_average', 1, 2, 'count_step', 'average_final')}
}

example 8 "Use the aggregate functions in ordinary SQL" {
$db do {create table numbers(x); insert into numbers values(1),(2),(3),(4);}
query $db {select bc_sum(x), bc_average(x) from numbers}
query $db {select bc_sum(x) from numbers where x >= 3}
}

example 9 "Register and use a true window function" {
query $db {select sc_bclite_register_window('bc_window_sum', 1, 1, 'sum_step', 'sum_inverse', 'sum_final', 'sum_final')}
query $db {select x, bc_window_sum(x) over (order by x rows between 1 preceding and current row) from numbers order by x}
}

example 10 "Change the exact-number text prefix" {
query $db {select sc_bclite_prefix('@')}
query $db {select twice('@1.25'), square('@2.5')}
query $db {select sc_bclite_prefix('#')}
}

example 11 "Use bound Tcl values safely in SQL" {
set input #123.456
puts "Tcl variable input = $input"
query $db {select twice(?), rounded_even(?, 2)} $input $input
}

example 12 "Handle invalid numeric input as a JimTcl error" {
expected_error {$db query {select twice('not-a-number')}}
}

example 13 "Library procedures remain private implementation details" {
expected_error {$db query {select sc_bclite_register_scalar('bad', 's', 1)}}
}

example 14 "Close the database cleanly" {
$db close
puts "Database closed."
}
puts "\nTutorial completed successfully."
