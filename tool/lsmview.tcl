#!/bin/sh
# \
exec wish "$0" "$@"

package require Tk

#############################################################################
# Code to set up scrollbars for widgets. This is generic, boring stuff.
#
namespace eval autoscroll {
  proc scrollable {widget path args} {
    ::ttk::frame $path
    set w  [$widget ${path}.widget {*}$args]
    set vs [::ttk::scrollbar ${path}.vs]
    set hs [::ttk::scrollbar ${path}.hs -orient horizontal]
    grid $w  -row 0 -column 0 -sticky nsew
  
    grid rowconfigure    $path 0 -weight 1
    grid columnconfigure $path 0 -weight 1
  
    set grid [list grid $vs -row 0 -column 1 -sticky nsew]
    $w configure -yscrollcommand [list ::autoscroll::scrollcommand $grid $vs]
    $vs configure -command       [list $w yview]
    set grid [list grid $hs -row 1 -column 0 -sticky nsew]
    $w configure -xscrollcommand [list ::autoscroll::scrollcommand $grid $hs]
    $hs configure -command       [list $w xview]
  
    return $w
  }
  proc scrollcommand {grid sb args} {
    $sb set {*}$args
    set isRequired [expr {[lindex $args 0] != 0.0 || [lindex $args 1] != 1.0}]
    if {$isRequired && ![winfo ismapped $sb]} {
      {*}$grid
    }
    if {!$isRequired && [winfo ismapped $sb]} {
      grid forget $sb
    }
  }
  namespace export scrollable
}
namespace import ::autoscroll::*
#############################################################################

#############################################################################
# Constants used by the code that draws the graphical representation of
# the data structure (the boxes with the arrows and stuff). Changing these
# will change the space left between various elements on the canvas.
#
set G(segment_height)    20
set G(vertical_padding)  40
set G(svertical_padding)  5
#############################################################################


# Return the base-2 log of the value passed as the only argument.
#
proc log2 {i} {
  return [expr {log($i) / log(2)}]
}

# The following procs:
#
#   varset
#   link_varset
#   del_varset
#
# are used to partition up the contents of global array VARSET so that it
# can be shared between multiple widget objects.
#
proc varset {C nm args} {
  global VARSET
  if {[llength $args] > 1} { error "should be: varset WIDGET VAR ?VALUE?" }
  eval set [list VARSET($C.$nm)] $args
}
proc del_varset {C} {
  global VARSET
  foreach k [array names $C*] { unset VARSET($k) }
}
proc link_varset {C args} {
  foreach nm $args { uplevel upvar ::VARSET($C.$nm) $nm }
}

proc draw_segment {C segment tags} {
  set nSize [lindex $segment 5]
  set bSep  [expr [lindex $segment 2]!=0]

  set iRunFirst [lindex $segment 3]
  set iSepFirst [lindex $segment 0]

  set w [expr {$::G(scale) * [log2 [expr max($nSize, 2)]]}]
  set h $::G(segment_height)

  set y 0

  # Draw the separators stack if required.
  if {$bSep} {
    set septag "[lindex $tags end].sep"
    set st [concat $tags $septag]
    set w2 $w
    for {set i 3} {$i > 0} {incr i -1} {
      set w2 [expr $w/pow(2,$i)]
      set h2 [expr $h/2]

      set x1 [expr ($w-$w2)/2]
      set x2 [expr $x1+$w2]

      set id [$C create rect $x1 $y $x2 [expr $y+$h2]]
      $C itemconfigure $id -outline black -fill white -tags $st
      incr y $h2
    }

    $C bind $septag <1> [list segment_callback $C $septag $iSepFirst]
  }

  set maintag "[lindex $tags end].main"
  set rt [concat $tags $maintag]
  $C create rectangle 0 $y $w [expr $y+$h] -outline black -fill white -tags $rt

  set tid [$C create text [expr $w/2] [expr $y+$h/2]]
  $C itemconfigure $tid -text "$nSize pages" -anchor center
  $C itemconfigure $tid -tags [concat $tags "[lindex $tags 0].text"]

  $C bind $maintag <1> [list segment_callback $C $maintag $iRunFirst]
  $C bind $tid <1>     [list segment_callback $C $maintag $iRunFirst]
}

proc segment_callback {C tag pgno} {
  link_varset $C mySelected myScript

  if {[info exists mySelected]} { $C itemconfigure $mySelected -fill white }
  set mySelected $tag
  $C itemconfigure $mySelected -fill grey

  eval $myScript $pgno
}

proc draw_level {C level tags} {
  set lhs [lindex $level 0]

  set l [lindex $tags end]
  draw_segment $C $lhs [concat $tags "$l.s0"]
  foreach {x1 y1 x2 y2} [$C bbox "$l.s0"] {}

  set i 0
  set y 0
  set x [expr $x2+10]
  foreach seg [lrange $level 1 end] {
    set tag "$l.s[incr i]"
    draw_segment $C $seg [concat $tags $tag]

    $C move $tag $x $y
    foreach {x1 y1 x2 y2} [$C bbox $tag] {}
    set y [expr $y2+$::G(vertical_padding)]
  }
}

proc draw_structure {canvas structure} {
  link_varset $canvas mySelected

  set C $canvas
  set W [winfo width $C]

  # Figure out the scale to use.
  # 
  set nMaxWidth 0.0
  foreach level $structure {
    set nRight 0
    foreach seg [lrange $level 1 end] {
      set sz [lindex $seg 5]
      if {$sz > $nRight} { set nRight $sz }
    }
    set nLeft [lindex $level 0 5]
    set nTotal [log2 [expr max($nLeft, 2)]]
    if {$nRight} {set nTotal [expr $nTotal + [log2 [expr max($nRight, 2)]]]}
    if {$nTotal > $nMaxWidth} { set nMaxWidth $nTotal }
  }

  if { $nMaxWidth==0.0 } { set nMaxWidth 1.0 }
  set ::G(scale) [expr (($W-100) / $nMaxWidth)]

  set y [expr $::G(vertical_padding) / 2]
  set l -1
  foreach level $structure {
    set tag "l[incr l]"

    draw_level $C $level $tag
    foreach {x1 y1 x2 y2} [$C bbox $tag] {}
    $C move $tag [expr ($W-$x2)/2] $y
    incr y [expr $y2 + $::G(vertical_padding)]

    foreach {x1 y1 x2 y2} [$C bbox $tag.text] {}
    $C create text 10 $y1 -anchor nw -text [string toupper "${tag}:"]
  }

  set H [winfo height $C]
  set region [$C bbox all]
  if {$y < $H} { $C move all 0 [expr $H-$y] }

  set region [$C bbox all]
  lset region 0 0
  lset region 1 0
  $C configure -scrollregion $region

  if {[info exists mySelected]} { $C itemconfigure $mySelected -fill grey }
}

proc draw_one_pointer {C from to} {
  foreach {xf1 yf1 xf2 yf2} [$C bbox $from] {}
  foreach {xt1 yt1 xt2 yt2} [$C bbox $to] {}

  set line [$C create line [expr ($xf1+$xf2)/2] $yf2 [expr ($xt1+$xt2)/2] $yt1]
  $C itemconfigure $line -arrow last 
}

proc draw_internal_pointers {C iLevel level} {
  for {set j 2} {$j < [llength $level]} {incr j} {
    if {[lindex $level $j 2]==0} {
      draw_one_pointer $C "l$iLevel.s[expr $j-1]" "l$iLevel.s$j"
    }
  }
}

proc draw_pointers {C structure} {
  for {set i 0} {$i < [llength $structure]} {incr i} {
    draw_internal_pointers $C $i [lindex $structure $i]
  }

  for {set i 0} {$i < ([llength $structure]-1)} {incr i} {
    set i2 [expr $i+1]

    set l1 [lindex $structure $i]
    set l2 [lindex $structure $i2]

    set bMerge1 [expr [llength $l1]>1]
    set bMerge2 [expr [llength $l2]>1]

    if {$bMerge2} {
      if {[lindex $l2 1 2]==0} {
        draw_one_pointer $C "l$i.s0" "l$i2.s1"
        if {$bMerge1} {
          draw_one_pointer $C "l$i.s[expr [llength $l1]-1]" "l$i2.s1"
        }
      }
    } else {
      set bBtree [expr [lindex $l2 0 2]!=0]

      if {$bBtree==0 || $bMerge1} {
        draw_one_pointer $C "l$i.s0" "l$i2.s0"
      }
      if {$bBtree==0} {
        draw_one_pointer $C "l$i.s[expr [llength $l1]-1]" "l$i2.s0"
      }
    }
  }
}

# Argument $canvas is a canvas widget. Argument $db is the Tcl data 
# structure returned by an LSM_INFO_DB_STRUCTURE call. This procedure
# clears the canvas widget, then draws a diagram representing the
# contents of $db. The canvas viewable region is set appropriately.
#
# The canvas widget is also configured with bindings so that the user
# may select any main or separators array using the pointer. Each time
# the selection changes, the page number of the first page in the selected
# array is appended to the list $script and the result evaulated.
#
proc draw_canvas_content {canvas db script} {
  link_varset $canvas myScript
  set myScript $script
  draw_structure $canvas $db
  draw_pointers $canvas $db
}

# End of drawing code.
##########################################################################

proc dynamic_redraw {C S args} {
  $C delete all
  set db [lindex $::G(data) [$S get]]
  if {[llength $db]>0} {
    draw_canvas_content $C $db dynamic_callback
  }
}

proc dynamic_callback {args} {}

proc dynamic_input {C S args} {
  fileevent stdin readable {}
  if {[eof stdin]} return

  set line [gets stdin]
  if {[llength $line] > 0 && $line != [lindex $::G(data) end]} { 
    lappend ::G(data) $line 
  }

  set end [expr [llength $::G(data)] - 1]
  if {$end>=0} { 
    $S configure -from 0 -to $end
    if {[$S get]==$end-1} { $S set $end }
  }

  fileevent stdin readable [list dynamic_input $C $S]
}

proc dynamic_setup {} {
  global C S

  set C [scrollable canvas .c -background white -width 600 -height 600]
  set S [scale .s -orient horiz]
  pack .s -side bottom -fill x
  pack .c -expand 1 -fill both

  bind $C <Configure>  [list dynamic_redraw $C $S]
  bind $C <KeyPress-q> exit
  focus $C
  $S configure -command [list dynamic_redraw $C $S]
  update
}

set ::G(data)    [list {}]


proc static_redraw {C} {
  link_varset $C myText myDb myData
  $C delete all
  draw_canvas_content $C $myData [list static_select_callback $C]
}

proc static_select_callback {C pgno} {
  link_varset $C myText myDb myData myTree
  set data [string trim [exec lsmtest show $myDb array $pgno]]
  $myText delete 0.0 end

  $myTree delete [$myTree children {}]

  foreach {first last} $data {
    set nPg [expr {$last - $first + 1}]
    set id [$myTree insert {} end -text "${first}..${last} ($nPg pages)"]
    for {set i $first} {$i <= $last} {incr i} {
      $myTree insert $id end -text $i
    }
  }

  $myTree item $id -open 1
  $myTree selection set [lindex [$myTree children $id] 0]
}

proc static_page_callback {C pgno} {
  link_varset $C myText myDb myData
  set data [string trim [exec lsmtest show $myDb page $pgno]]
  $myText delete 0.0 end
  $myText insert 0.0 $data
}

proc static_treeview_select {C} {
  link_varset $C myText myDb myData myTree
  set sel [lindex [$myTree selection] 0]
  if {$sel != ""} {
    set text [$myTree item $sel -text]
    if {[string is integer $text]} { static_page_callback $C $text }
  }
}

proc static_setup {zDb} {

  panedwindow .pan -orient horizontal
  set C [scrollable canvas .pan.c -background white -width 400 -height 600]

  link_varset $C myText myDb myData myTree mySelected
  set myDb $zDb
  set myText [scrollable text .pan.t -background white -width 80]
  set myTree [scrollable ::ttk::treeview .pan.tree]
  set myData [string trim [exec lsmtest show $zDb]]

  $myText configure -wrap none
  bind $C <KeyPress-q> exit
  focus $C

  .pan add .pan.c .pan.tree .pan.t
  pack .pan -fill both -expand 1

  set mySelected "l0.s0.main"
  static_redraw $C
  bind $C <Configure>  [list static_redraw $C]
  bind $myTree <<TreeviewSelect>> [list static_treeview_select $C]
  static_select_callback $C [lindex $myData 0 0 3]
}

if {[llength $argv] > 1} {
  puts stderr "Usage: $argv0 ?DATABASE?"
  exit -1
}
if {[llength $argv]==1} {
  static_setup [lindex $argv 0]
} else {
  dynamic_setup
  fileevent stdin readable [list dynamic_input $C $S]
}

