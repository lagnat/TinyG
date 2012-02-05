/*
 * gcode_interpreter.c - rs274/ngc parser.
 * Part of TinyG project
 *
 * Copyright (c) 2010-2011 Alden S. Hart, Jr.
 * Portions copyright (c) 2009 Simen Svale Skogsrud
 *
 * This interpreter attempts to follow the NIST RS274/NGC 
 * specification as closely as possible with regard to order 
 * of operations and other behaviors.
 *
 * Copyright (c) 2010 - 2011 Alden S. Hart Jr.
 *
 * TinyG is free software: you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, 
 * or (at your option) any later version.
 *
 * TinyG is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 * See the GNU General Public License for details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with TinyG  If not, see <http://www.gnu.org/licenses/>.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* See http://www.synthetos.com/wiki/index.php?title=Projects:TinyG-Developer-Info
 */

#include <ctype.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>				// needed for memcpy, memset
#include <stdio.h>				// precursor for xio.h
#include <avr/pgmspace.h>		// precursor for xio.h

#include "tinyg.h"
#include "util.h"
#include "config.h"
#include "controller.h"
#include "gcode_parser.h"
#include "canonical_machine.h"
#include "planner.h"
#include "help.h"
#include "xio/xio.h"				// for char definitions

/* local helper functions and macros */
static void _gc_normalize_gcode_block(char *block);
static uint8_t _gc_parse_gcode_block(char *line);	// Parse the block into structs
static uint8_t _gc_execute_gcode_block(void);		// Execute the gcode block
static uint8_t _get_next_statement(char *letter, double *value_ptr, double *fraction_ptr,  char *buf, uint8_t *i);

#define ZERO_MODEL_STATE(g) memset(g, 0, sizeof(struct GCodeModel))
#define SET_NEXT_STATE(a,v) ({gn.a=v; gf.a=1; break;})
#define SET_NEXT_STATE_x2(a,v,b,w) ({gn.a=v; gf.a=1; gn.b=w; gf.a=1; break;})
#define SET_NEXT_ACTION_MOTION(a,v) ({gn.a=v; gf.a=1; gn.next_action=NEXT_ACTION_MOTION; gf.next_action=1; break;})
#define SET_NEXT_ACTION_OFFSET(a,v) ({gn.a=v; gf.a=1; gn.next_action=NEXT_ACTION_OFFSET_COORDINATES; gf.next_action=1; break;})
#define CALL_CM_FUNC(f,v) if((int)gf.v != 0) { if ((status = f(gn.v)) != TG_OK) { return(status); } }
/* Derived from::
	if ((int)gf.feed_rate != 0) {		// != 0 either means true or non-zero value
		if ((status = cm_set_feed_rate(gn.feed_rate)) != TG_OK) {
			return(status);				// error return
		}
	}
 */

/* 
 * gc_init() 
 */

void gc_init()
{
	cm_init();							// initialize canonical machine
}

/*
 * gc_gcode_parser() - parse a block (line) of gcode
 *
 *	Top level of gcode parser. Normalizes block and looks for special cases
 */

uint8_t gc_gcode_parser(char *block)
{
	_gc_normalize_gcode_block(block);		// get block ready for parsing
	if (block[0] == NUL) return (TG_NOOP); 	// ignore comments (stripped)
	return(_gc_parse_gcode_block(block));	// parse block & return status
}

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

static void _gc_normalize_gcode_block(char *block) 
{
	char c;
	char *comment=0;	// comment pointer - first char past opening paren
	uint8_t i=0; 		// index for incoming characters
	uint8_t j=0;		// index for normalized characters

	if (block[0] == '/') {					// discard deleted blocks
		block[0] = NUL;
		return;
	}
	if (block[0] == '?') {					// trap and return ? command
		return;
	}
	// normalize the command block & mark the comment(if any)
	while ((c = toupper(block[i++])) != NUL) {
		if ((isupper(c)) || (isdigit(c))) {	// capture common chars
		 	block[j++] = c; 
			continue;
		}
		if (c == '(') {						// detect & handle comments
			block[j] = NUL;
			comment = &block[i]; 
			break;
		}
		if (c <= ' ') continue;				// toss controls & whitespace
		if (c == DEL) continue;				// toss DELETE (0x7F)
		if (strchr("!$%,;:?@^_~`\'\"", c))	// toss invalid punctuation
			continue;
		block[j++] = c;
	}
	block[j] = NUL;							// terminate the command
	if (comment != 0) {
		if ((toupper(comment[0]) == 'M') && 
			(toupper(comment[1]) == 'S') &&
			(toupper(comment[2]) == 'G')) {
			i=0;
			while ((c = comment[i++]) != NUL) {// remove trailing parenthesis
				if (c == ')') {
					comment[--i] = NUL;
					break;
				}
			}
			(void)cm_message(comment+3);
		}
	}
	cm.linecount += 1;
}

/* 
 * _gc_next_statement() - parse next statement in a block of Gcode
 *
 *	Parses the next statement and leaves the index on the first 
 *	character following the statement. 
 *	Returns TG_OK if there was a statement, some other code if not.
 */

static uint8_t _get_next_statement(char *letter, double *value_ptr, double *fraction_ptr,  char *buf, uint8_t *i) {
	if (buf[*i] == NUL) { 		// no more statements
		return (TG_COMPLETE);
	}
	*letter = buf[*i];
	if(isupper(*letter) == false) { 
		return (TG_EXPECTED_COMMAND_LETTER);
	}
	(*i)++;
	if (read_double(buf, i, value_ptr) == false) {
		return (TG_BAD_NUMBER_FORMAT);
	}
	*fraction_ptr = (*value_ptr - trunc(*value_ptr));
	return (TG_OK);
}

/*
 * _gc_parse_gcode_block() - parses one line of NULL terminated G-Code. 
 *
 *	All the parser does is load the state values in gn (next model state),
 *	and flags in gf (model state flags). The execute routine applies them.
 *	The line is assumed to contain only uppercase characters and signed 
 *  floats (no whitespace).
 *
 *	A number of implicit things happen when the gn struct is zeroed:
 *	  - inverse feed rate mode is cancelled - set back to units_per_minute mode
 */

static uint8_t _gc_parse_gcode_block(char *buf) 
{
	uint8_t i=0; 	 			// persistent index into Gcode block buffer (buf)
  	char letter;				// parsed letter, eg.g. G or X or Y
	double value;				// value parsed from letter (e.g. 2 for G2)
	double fraction;			// value fraction, eg. 0.1 for 64.1
	uint8_t status = TG_OK;

	ZERO_MODEL_STATE(&gn);		// clear all next-state values
	ZERO_MODEL_STATE(&gf);		// clear all next-state flags

	// pull needed state from gm structure to preset next state
	gn.next_action = cm_get_next_action();	// next action persists
	gn.motion_mode = cm_get_motion_mode();	// motion mode (G modal group 1)
	gn.absolute_mode = cm_get_absolute_mode();
	cm_set_absolute_override(FALSE);		// must be set per block 

  	// extract commands and parameters
	while((status = _get_next_statement(&letter, &value, &fraction, buf, &i)) == TG_OK) {
		switch(letter) {
			case 'G':
				switch((uint8_t)value) {
					case 0:  SET_NEXT_ACTION_MOTION(motion_mode, MOTION_MODE_STRAIGHT_TRAVERSE);
					case 1:  SET_NEXT_ACTION_MOTION(motion_mode, MOTION_MODE_STRAIGHT_FEED);
					case 2:  SET_NEXT_ACTION_MOTION(motion_mode, MOTION_MODE_CW_ARC);
					case 3:  SET_NEXT_ACTION_MOTION(motion_mode, MOTION_MODE_CCW_ARC);
					case 4:  SET_NEXT_STATE(next_action, NEXT_ACTION_DWELL);
					case 17: SET_NEXT_STATE(select_plane, CANON_PLANE_XY);
					case 18: SET_NEXT_STATE(select_plane, CANON_PLANE_XZ);
					case 19: SET_NEXT_STATE(select_plane, CANON_PLANE_YZ);
					case 20: SET_NEXT_STATE(inches_mode, true);
					case 21: SET_NEXT_STATE(inches_mode, FALSE);
					case 28: SET_NEXT_STATE(next_action, NEXT_ACTION_RETURN_TO_HOME);
					case 30: SET_NEXT_STATE(next_action, NEXT_ACTION_HOMING_CYCLE);
					case 53: SET_NEXT_STATE(absolute_override, true);
					case 61: SET_NEXT_STATE(path_control, PATH_EXACT_PATH);
					case 64: SET_NEXT_STATE(path_control, PATH_CONTINUOUS);
					case 80: SET_NEXT_STATE(motion_mode, MOTION_MODE_CANCEL_MOTION_MODE);
					case 90: SET_NEXT_STATE(absolute_mode, true);
					case 91: SET_NEXT_STATE(absolute_mode, FALSE);
					case 92: SET_NEXT_ACTION_OFFSET(set_origin_mode, true);
					case 93: SET_NEXT_STATE(inverse_feed_rate_mode, true);
					case 94: SET_NEXT_STATE(inverse_feed_rate_mode, FALSE);
					case 40: break;	// ignore cancel cutter radius compensation
					case 49: break;	// ignore cancel tool length offset comp.
					default: status = TG_UNRECOGNIZED_COMMAND;
				}
				break;

			case 'M':
				switch((uint8_t)value) {
					case 0: case 1: 
							SET_NEXT_STATE(program_flow, PROGRAM_FLOW_PAUSED);
					case 2: case 30: case 60:
							SET_NEXT_STATE(program_flow, PROGRAM_FLOW_COMPLETED);
					case 3: SET_NEXT_STATE(spindle_mode, SPINDLE_CW);
					case 4: SET_NEXT_STATE(spindle_mode, SPINDLE_CCW);
					case 5: SET_NEXT_STATE(spindle_mode, SPINDLE_OFF);
					case 6: SET_NEXT_STATE(change_tool, true);
					case 7: break;	// ignore mist coolant on
					case 8: break;	// ignore flood coolant on
					case 9: break;	// ignore mist and flood coolant off
					case 48: break;	// enable speed and feed overrides
					case 49: break;	// disable speed and feed overrides
					default: status = TG_UNRECOGNIZED_COMMAND;
				}
				break;

			case 'T': SET_NEXT_STATE(tool, (uint8_t)trunc(value));
			case 'F': SET_NEXT_STATE(feed_rate, value);
			case 'P': SET_NEXT_STATE(dwell_time, value);
			case 'S': SET_NEXT_STATE(spindle_speed, value); 
			case 'X': SET_NEXT_STATE(target[X], value);
			case 'Y': SET_NEXT_STATE(target[Y], value);
			case 'Z': SET_NEXT_STATE(target[Z], value);
			case 'A': SET_NEXT_STATE(target[A], value);
			case 'B': SET_NEXT_STATE(target[B], value);
			case 'C': SET_NEXT_STATE(target[C], value);
		//	case 'U': SET_NEXT_STATE(target[U], value);	// reserved
		//	case 'V': SET_NEXT_STATE(target[V], value);	// reserved
		//	case 'W': SET_NEXT_STATE(target[W], value);	// reserved
			case 'I': SET_NEXT_STATE(arc_offset[0], value);
			case 'J': SET_NEXT_STATE(arc_offset[1], value);
			case 'K': SET_NEXT_STATE(arc_offset[2], value);
			case 'R': SET_NEXT_STATE(arc_radius, value);
			case 'N': cm.linenum = (uint32_t)value; break;	// save line #
			default: status = TG_UNRECOGNIZED_COMMAND;
		}
		// Process supported fractional gcodes
		if ((letter == 'G') && ((uint8_t)value == 61) && ((uint8_t)fraction != 0)) {	// 61.1
			SET_NEXT_STATE(path_control, PATH_EXACT_STOP);
		}
		if ((letter == 'G') && ((uint8_t)value == 92) && ((uint8_t)fraction != 0)) {	// 92.1
			for (i=0; i<AXES; i++) { SET_NEXT_STATE(target[i], 0);}
		}
		if(status != TG_OK) break;
	}
	// set targets correctly. fill-in any unset target if in absolute mode, 
	// otherwise leave the target values alone
	for (i=0; i<AXES; i++) {
		if (((gn.absolute_mode == true) || 
			 (gn.absolute_override == true)) && 
			 (gf.target[i] < EPSILON)) {		
			gn.target[i] = cm_get_position(i); // get target from model
		}
	}
	return (_gc_execute_gcode_block());
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

static uint8_t _gc_execute_gcode_block() 
{
	uint8_t status = TG_OK;

	CALL_CM_FUNC(cm_set_inverse_feed_rate_mode, inverse_feed_rate_mode);
	CALL_CM_FUNC(cm_set_feed_rate, feed_rate);
	CALL_CM_FUNC(cm_set_spindle_speed, spindle_speed);
	CALL_CM_FUNC(cm_select_tool, tool);
	CALL_CM_FUNC(cm_change_tool, tool);

	// spindle on or off
	if (gf.spindle_mode == true) {
    	if (gn.spindle_mode == SPINDLE_CW) {
			(void)cm_start_spindle_clockwise();
		} else if (gn.spindle_mode == SPINDLE_CCW) {
			(void)cm_start_spindle_counterclockwise();
		} else {
			(void)cm_stop_spindle_turning();	// failsafe: any error causes stop
		}
	}

 	//--> coolant on or off goes here
	//--> enable or disable overrides goes here

	// dwell
	if (gn.next_action == NEXT_ACTION_DWELL) { 
		ritorno(cm_dwell(gn.dwell_time));
	}

	// select plane
	CALL_CM_FUNC(cm_select_plane, select_plane);

	// use units (inches mode / mm mode)
	if (gf.inches_mode == true) {
		return(cm_set_inches_mode(gn.inches_mode));
	}

	//--> cutter radius compensation goes here
	//--> cutter length compensation goes here
	//--> coordinate system selection goes here
	//--> set path control mode goes here

	CALL_CM_FUNC(cm_set_absolute_mode, absolute_mode);

	//--> set retract mode goes here

	// soft home - return to zero (G28)
	if (gn.next_action == NEXT_ACTION_RETURN_TO_HOME) {
		ritorno(cm_return_to_home());
	}

	// hard home - initiate a homing cycle (G30)
	if (gn.next_action == NEXT_ACTION_HOMING_CYCLE) {
		ritorno(cm_homing_cycle());
	}

	//--> change coordinate system data goes here

	// set axis offsets (G92)
	if (gn.next_action == NEXT_ACTION_OFFSET_COORDINATES) {
		ritorno(cm_set_origin_offsets(gn.target));
	}

	// G0 (linear traverse motion command)
	if ((gn.next_action == NEXT_ACTION_MOTION) && (gn.motion_mode == MOTION_MODE_STRAIGHT_TRAVERSE)) {
		return (cm_straight_traverse(gn.target));
	}

	// G1 (linear feed motion command)
	if ((gn.next_action == NEXT_ACTION_MOTION) && (gn.motion_mode == MOTION_MODE_STRAIGHT_FEED)) {
		return (cm_straight_feed(gn.target));
	}

	// G2 or G3 (arc motion command)
	if ((gn.next_action == NEXT_ACTION_MOTION) &&
	   ((gn.motion_mode == MOTION_MODE_CW_ARC) || (gn.motion_mode == MOTION_MODE_CCW_ARC))) {
		// gf.radius sets radius mode if radius was collected in gn
		return (cm_arc_feed(gn.target, gn.arc_offset[0], gn.arc_offset[1], gn.arc_offset[2], 
							gn.arc_radius, gn.motion_mode));
	}
	return (status);
}