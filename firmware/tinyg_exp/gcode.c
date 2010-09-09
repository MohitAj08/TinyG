/*
 * gcode_interpreter.c - rs274/ngc parser.
 * Adapted from Grbl
 * This code is inspired by the Arduino GCode Interpreter by Mike Ellery and 
 * the NIST RS274/NGC Interpreter by Kramer, Proctor and Messina. 
 *
 * Copyright (c) 2009 Simen Svale Skogsrud
 * Modified for TinyG project by Alden S Hart, Jr.
 *
 * Grbl is free software: you can redistribute it and/or modify it under the 
 * termsof the GNU General Public License as published by the Free Software 
 * Foundation, either version 3 of the License, or (at your option) any later 
 * version.
 *
 * Grbl is distributed in the hope that it will be useful, but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along 
 * with Grbl. If not, see <http://www.gnu.org/licenses/>.
 *
 * ---> See end of gcode.h file for list of supported commands and a
 *		discussion of some other details.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>					// for memset()
#include <math.h>
#include <stdio.h>
#include <avr/pgmspace.h>			// needed for exception strings

#include "tinyg.h"
#include "gcode.h"					// must precede config.h
#include "config.h"
#include "controller.h"
#include "motion_control.h"
#include "canonical_machine.h"
#include "spindle.h"

/* 
 * Data structures 
 *
 * - gp is a minimal structure to keep parser state
 *
 * The three GCodeModel structs look the same but have different uses:
 *
 * - gm keeps the internal state model in normalized, canonical form. All
 * 	 values are unit converted (to mm) and in the internal coordinate system.
 *	 Gm is owned by the canonical motion layer and is accessed by the 
 *	 parser through cm_ routines (which also include various setters and 
 *	 getters). Gm's state persists throughout the program.
 *
 * - gn records the data in the new gcode block in the formats present in
 *	 the block (pre-normalized forms). It is used by the gcode interpreter 
 *	 and is initialized for each block. During initialization some state 
 *	 elements are necessarily restored from gm.
 *
 * - gf is used by the interpreter to hold flag for any data that has changed
 *	 in gn durint the parse. 
 */
static struct GCodeParser gp;		// gcode parser variables
//static struct GCodeModel gm;		// (see conaonical machine.c)
static struct GCodeModel gn;		// gcode model - current block values
static struct GCodeModel gf;		// gcode model - flags changed values

/* local helper functions and macros */
static void _gc_normalize_gcode_block(char *block);
static int _gc_parse_gcode_block(char *line);	// Parse the block into structs
static int _gc_execute_gcode_block(void);		// Execute the gcode block
static int _gc_read_double(char *buf, uint8_t *i, double *double_ptr);
static int _gc_next_statement(char *letter, double *value_ptr, double *fraction_ptr, char *line, uint8_t *i);

#define ZERO_MODEL_STATE(g) memset(g, 0, sizeof(struct GCodeModel))
#define SET_NEXT_STATE(a,v) ({gn.a=v; gf.a=1; break;})
#define SET_NEXT_STATE_x2(a,v,b,w) ({gn.a=v; gf.a=1; gn.b=w; gf.a=1; break;})
#define SET_NEXT_ACTION_MOTION(a,v) ({gn.a=v; gf.a=1; gn.next_action=NEXT_ACTION_MOTION; gf.next_action=1; break;})

/* 
 * gc_init() 
 */

void gc_init()
{
	ZERO_MODEL_STATE(&gn);
	ZERO_MODEL_STATE(&gf);
	cm_init_canon();						// initialize canonical machine
}

/*
 * gc_gcode_parser() - parse a block (line) of gcode
 */

uint8_t gc_gcode_parser(char *block)
{
	_gc_normalize_gcode_block(block);
	if (block[0] == 0) { 					// ignore comments (stripped)
		return(TG_OK);
	}
	if (block[0] == 'Q') {					// quit gcode mode (see note 1)
		return(TG_QUIT);
	}
	if (_gc_parse_gcode_block(block)) { // parse & exec block or fail trying
		tg_print_status(gp.status, block);
	}
	return (gp.status);
}
// Note 1:	Q is the feed_increment value for a peck drilling (G83) cycle. 
//			So you might have to change this if you implement peck drilling
//			or expect to see a leading Q value in a CGode file/block.

/*
 * _gc_normalize_gcode_block() - normalize a block (line) of gcode in place
 *
 *	Comments always terminate the block (embedded comments are not supported)
 *	Messages in comments are sent to console (stderr)
 *	Processing: split string into command and comment portions. Valid choices:
 *	  supported:	command
 *	  supported:	comment
 *	  supported:	command comment
 *	  unsupported:	command command
 *	  unsupported:	comment command
 *	  unsupported:	command comment command
 *
 *	Valid characters in a Gcode block are (see RS274NGC_3 Appendix E)
 *		digits						all digits are passed to interpreter
 *		lower case alpha			all alpha is passed
 *		upper case alpha			all alpha is passed
 *		+ - . / *	< = > 			chars passed to interpreter
 *		| % # ( ) [ ] { } 			chars passed to interpreter
 *		<sp> <tab> 					chars are legal but are not passed
 *		/  							if first, block delete char - omits the block
 *
 *	Invalid characters in a Gcode block are:
 *		control characters			chars < 0x20
 *		! $ % ,	; ; ? @ 
 *		^ _ ~ " ' <DEL>
 *
 *	MSG specifier in comment can have mixed case but cannot cannot have 
 *	embedded white spaces
 *
 *	++++ todo: Support leading and trailing spaces around the MSG specifier
 */

void _gc_normalize_gcode_block(char *block) 
{
	char c;
	char *comment=0;	// comment pointer - first char past opening paren
	uint8_t i=0; 		// index for incoming characters
	uint8_t j=0;		// index for normalized characters

	// discard deleted block
	if (block[0] == '/') {
		block[0] = 0;
		return;
	}
	// normalize the comamnd block & mark the comment(if any)
	while ((c = toupper(block[i++])) != 0) {// NUL character
		if ((isupper(c)) || (isdigit(c))) {	// capture common chars
		 	block[j++] = c; 
			continue;
		}
		if (c == '(') {						// detect & handle comments
			block[j] = 0;
			comment = &block[i]; 
			break;
		}
		if (c <= ' ') continue;				// toss controls & whitespace
		if (c == 0x7F) continue;			// toss DELETE
		if (strchr("!$%,;:?@^_~`\'\"", c))	// toss invalid punctuation
			continue;
		block[j++] = c;
	}
	block[j] = 0;							// nul terminate the command
	if (comment) {
		if ((toupper(comment[0]) == 'M') && 
			(toupper(comment[1]) == 'S') &&
			(toupper(comment[2]) == 'G')) {
			i=0;
			while ((c = comment[i++]) != 0) {// remove trailing parenthesis
				if (c == ')') {
					comment[--i] = 0;
					break;
				}
			}
			cm_message(comment+3);
		}
	}
}

/* 
 * _gc_next_statement() - parse next block of Gcode
 *
 *	Parses the next statement and leaves the counter on the first character 
 *	following the statement. 
 *	Returns TRUE if there was a statement, FALSE if end of string was reached
 *	or there was an error (check gp.status).
 */

int _gc_next_statement(char *letter, double *value_ptr, double *fraction_ptr, 
					   char *buf, uint8_t *i) {
	if (buf[*i] == 0) {
		return(FALSE); // No more statements
	}
	*letter = buf[*i];
	if(!isupper(*letter)) {
		gp.status = TG_EXPECTED_COMMAND_LETTER;
		return(FALSE);
	}
	(*i)++;
	if (!_gc_read_double(buf, i, value_ptr)) {
		return(FALSE);
	};
	*fraction_ptr = (*value_ptr - trunc(*value_ptr));
	return(TRUE);
}

/* 
 * _gc_read_double() - read a double from a Gcode statement 
 *
 *	buf			string: line of RS274/NGC code being processed
 *	i			index into string array (position on the line)
 *	double_ptr	pointer to double to be read
 */

int _gc_read_double(char *buf, uint8_t *i, double *double_ptr) 
{
	char *start = buf + *i;
	char *end;
  
	*double_ptr = strtod(start, &end);
	if(end == start) { 
		gp.status = TG_BAD_NUMBER_FORMAT; 
		return(FALSE); 
	};
	*i = end - buf;
	return(TRUE);
}

/*
 * _gc_parse_gcode_block() - parses one line of NULL terminated G-Code. 
 *
 *	All the parser does is load the state values in gn (next model state),
 *	and flags in gf (model state flags). The execute routine applies them.
 *	The line is assumed to contain only uppercase characters and signed 
 *  floats (no whitespace).
 *
 *	A lot of implicit things happen when the gn struct is zeroed:
 *	  - inverse feed rate mode is cancelled - set back to units_per_minute mode
 */

int _gc_parse_gcode_block(char *buf) 
{
	uint8_t i = 0;  			// index into Gcode block buffer (buf)
  
	ZERO_MODEL_STATE(&gn);		// clear all next-state values
	ZERO_MODEL_STATE(&gf);		// clear all next-state flags

	// pull needed state from gm structure to preset next state
	gn.next_action = cm_get_next_action();	// next action persists
	gn.motion_mode = cm_get_motion_mode();	// motion mode (G modal group 1)
	for (i=X; i<=Z; i++) {
		gn.target[i] = cm_get_position(i);	// pre-set the target values
//		gn.position[i] = gn.target[i];		//...and current position
	}

	gp.status = TG_OK;						// initialize return code

  	// extract commands and parameters
	i = 0;
	while(_gc_next_statement(&gp.letter, &gp.value, &gp.fraction, buf, &i)) {
    	switch(gp.letter) {
			case 'G':
				switch((int)gp.value) {
					case 0:  SET_NEXT_ACTION_MOTION(motion_mode, MOTION_MODE_STRAIGHT_TRAVERSE);
					case 1:  SET_NEXT_ACTION_MOTION(motion_mode, MOTION_MODE_STRAIGHT_FEED);
					case 2:  SET_NEXT_ACTION_MOTION(motion_mode, MOTION_MODE_CW_ARC);
					case 3:  SET_NEXT_ACTION_MOTION(motion_mode, MOTION_MODE_CCW_ARC);
					case 4:  SET_NEXT_STATE(next_action, NEXT_ACTION_DWELL);
					case 17: SET_NEXT_STATE(set_plane, CANON_PLANE_XY);
					case 18: SET_NEXT_STATE(set_plane, CANON_PLANE_XZ);
					case 19: SET_NEXT_STATE(set_plane, CANON_PLANE_YZ);
					case 20: SET_NEXT_STATE(inches_mode, TRUE);
					case 21: SET_NEXT_STATE(inches_mode, FALSE);
					case 28: SET_NEXT_STATE(next_action, NEXT_ACTION_GO_HOME);
					case 30: SET_NEXT_STATE(next_action, NEXT_ACTION_GO_HOME);
					case 53: SET_NEXT_STATE(absolute_override, TRUE);
					case 80: SET_NEXT_STATE(motion_mode, MOTION_MODE_CANCEL_MOTION_MODE);
					case 90: SET_NEXT_STATE(absolute_mode, TRUE);
					case 91: SET_NEXT_STATE(absolute_mode, FALSE);
					case 92: SET_NEXT_STATE(set_origin_mode, TRUE);
					case 93: SET_NEXT_STATE(inverse_feed_rate_mode, TRUE);
					case 94: SET_NEXT_STATE(inverse_feed_rate_mode, FALSE);
					case 40: break;	// ignore cancel cutter radius compensation
					case 49: break;	// ignore cancel tool length offset comp.
					case 61: break;	// ignore set exact path (it is anyway)
					default: gp.status = TG_UNSUPPORTED_STATEMENT;
				}
				break;

			case 'M':
				switch((int)gp.value) {
					case 0: case 1: 
							SET_NEXT_STATE(program_flow, PROGRAM_FLOW_STOP);
					case 2: case 30: case 60:
							SET_NEXT_STATE(program_flow, PROGRAM_FLOW_END);
					case 3: SET_NEXT_STATE(spindle_mode, SPINDLE_CW);
					case 4: SET_NEXT_STATE(spindle_mode, SPINDLE_CCW);
					case 5: SET_NEXT_STATE(spindle_mode, SPINDLE_OFF);
					case 6: SET_NEXT_STATE(change_tool, TRUE);
					case 7: break;	// ignore mist coolant on
					case 8: break;	// ignore flood coolant on
					case 9: break;	// ignore mist and flood coolant off
					case 48: break;	// enable speed and feed overrides
					case 49: break;	// disable speed and feed overrides
 					default: gp.status = TG_UNSUPPORTED_STATEMENT;
				}
				break;

			case 'T': SET_NEXT_STATE(tool, trunc(gp.value));
			case 'F': SET_NEXT_STATE(feed_rate, gp.value);
			case 'P': SET_NEXT_STATE(dwell_time, gp.value);
			case 'S': SET_NEXT_STATE(spindle_speed, gp.value); 
			case 'X': SET_NEXT_STATE(target[X], gp.value);
			case 'Y': SET_NEXT_STATE(target[Y], gp.value);
			case 'Z': SET_NEXT_STATE(target[Z], gp.value);
			case 'I': SET_NEXT_STATE(offset[0], gp.value);
			case 'J': SET_NEXT_STATE(offset[1], gp.value);
			case 'K': SET_NEXT_STATE(offset[2], gp.value);
			case 'R': SET_NEXT_STATE(radius, gp.value);
			case 'N': break;	// ignore line numbers
			default: gp.status = TG_UNSUPPORTED_STATEMENT;
		}
		if(gp.status) {
			break;
		}
	}
	return (_gc_execute_gcode_block());
//	return(gp.status);
}

/*
 * _gc_execute_gcode_block() - execute parsed block
 *
 *  Conditionally (based on whether a flag is set in gf) call the canonical 
 *	machining functions in order of execution as per RS274NGC_3 table 8 
 *  (below, with modifications):
 *
 *		1. comment (includes message) [handled during block normalization]
 *		2. set feed rate mode (G93, G94 - inverse time or per minute)
 *		3. set feed rate (F)
 *		4. set spindle speed (S)
 *		5. select tool (T)
 *		6. change tool (M6)
 *		7. spindle on or off (M3, M4, M5)
 *		8. coolant on or off (M7, M8, M9)
 *		9. enable or disable overrides (M48, M49)
 *		10. dwell (G4)
 *		11. set active plane (G17, G18, G19)
 *		12. set length units (G20, G21)
 *		13. cutter radius compensation on or off (G40, G41, G42)
 *		14. cutter length compensation on or off (G43, G49)
 *		15. coordinate system selection (G54, G55, G56, G57, G58, G59, G59.1, G59.2, G59.3)
 *		16. set path control mode (G61, G61.1, G64)
 *		17. set distance mode (G90, G91)
 *		18. set retract mode (G98, G99)
 *		19a. home (G28, G30) or
 *		19b. change coordinate system data (G10) or
 *		19c. set axis offsets (G92, G92.1, G92.2, G94)
 *		20. perform motion (G0 to G3, G80-G89) as modified (possibly) by G53
 *		21. stop (M0, M1, M2, M30, M60)
 *
 *	Values in gn are in original units and should not be unit converted prior 
 *	to calling the canonical functions (which do the unit conversions)
 */

#define CALL_CM_FUNC(f,v) if(gf.v) {if((gp.status = f(gn.v))) {return(gp.status);}}
/* Example:
	if (gf.feed_rate) {
		if ((gp.status = cm_set_feed_rate(gn.feed_rate))) {
			return(gp.status);								// error return
		}
	}
 */

int _gc_execute_gcode_block() 
{
	CALL_CM_FUNC(cm_set_inverse_feed_rate_mode, inverse_feed_rate_mode);
	CALL_CM_FUNC(cm_set_feed_rate, feed_rate);
	CALL_CM_FUNC(cm_set_spindle_speed, spindle_speed);
	CALL_CM_FUNC(cm_select_tool, tool);
	CALL_CM_FUNC(cm_change_tool, tool);

	// spindle on or off
	if (gf.spindle_mode) {
    	if (gn.spindle_mode == SPINDLE_CW) {
			cm_start_spindle_clockwise();
		} else if (gn.spindle_mode == SPINDLE_CCW) {
			cm_start_spindle_counterclockwise();
		} else {
			cm_stop_spindle_turning();	// failsafe: any error causes stop
		}
	}

 	//--> coolant on or off goes here
	//--> enable or disable overrides goes here

	// dwell
	if (gn.next_action == NEXT_ACTION_DWELL) {
		if ((gp.status = cm_dwell(gn.dwell_time))) {
			return (gp.status);
		}
	}

	CALL_CM_FUNC(cm_select_plane, set_plane);
	CALL_CM_FUNC(cm_use_length_units, inches_mode);

	//--> cutter radius compensation goes here
	//--> cutter length compensation goes here
	//--> coordinate system selection goes here
	//--> set path control mode goes here

	CALL_CM_FUNC(cm_set_distance_mode, absolute_mode);

	//--> set retract mode goes here

	// homing cycle
	if (gn.next_action == NEXT_ACTION_GO_HOME) {
		if ((gp.status = cm_return_to_home())) {
			return (gp.status);								// error return
		}
	}

	//--> change coordinate system data goes here

	// set axis offsets
	if (gn.next_action == NEXT_ACTION_OFFSET_COORDINATES) {
		if ((gp.status = cm_set_origin_offsets(
						 gn.target[X], gn.target[Y], gn.target[Z]))) {
			return (gp.status);								// error return
		}
	}

	// G0 (linear traverse motion command)
	if ((gn.next_action == NEXT_ACTION_MOTION) && 
	    (gn.motion_mode == MOTION_MODE_STRAIGHT_TRAVERSE)) {
		gp.status = cm_straight_traverse(gn.target[X], gn.target[Y], gn.target[Z]);
		return (gp.status);
	}

	// G1 (linear feed motion command)
	if ((gn.next_action == NEXT_ACTION_MOTION) && 
	    (gn.motion_mode == MOTION_MODE_STRAIGHT_FEED)) {
		gp.status = cm_straight_feed(gn.target[X], gn.target[Y], gn.target[Z]);
		return (gp.status);
	}

	// G2 or G3 (arc motion command)
	if ((gn.next_action == NEXT_ACTION_MOTION) &&
	   ((gn.motion_mode == MOTION_MODE_CW_ARC) || 
		(gn.motion_mode == MOTION_MODE_CCW_ARC))) {
		// gf.radius sets radius mode if radius was collected in gn
		gp.status = cm_arc_feed(gn.target[X], gn.target[Y], gn.target[Z],
								gn.offset[0], gn.offset[1], gn.offset[2], 
								gn.radius, gn.motion_mode);
		return (gp.status);
	}
	return(gp.status);
}
