#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define DEBUG
#include "board.h"
#include "debug.h"
#include "random.h"
#include "tactics/selfatari.h"


struct selfatari_state {
	int groupcts[S_MAX];
	group_t groupids[S_MAX][4];

	/* This is set if this move puts a group out of _all_
	 * liberties; we need to watch out for snapback then. */
	bool friend_has_no_libs;
	/* We may have one liberty, but be looking for one more.
	 * In that case, @needs_more_lib is id of group
	 * already providing one, don't consider it again. */
	group_t needs_more_lib;
	/* ID of the first liberty, providing it again is not
	 * interesting. */
	coord_t needs_more_lib_except;
};

static bool
three_liberty_suicide(struct board *b, group_t g, enum stone color, coord_t to, struct selfatari_state *s)
{
	/* If a group has three liberties, by playing on one of
	 * them it is possible to kill the group clumsily. Check
	 * against that condition: "After our move, the opponent
	 * can unconditionally capture the group."
	 *
	 * Examples:
	 *
	 * O O O O O O O   X X O O O O O O     v-v- ladder
	 * O X X X X X O   . O X X X X X O   . . . O O
	 * O X ! . ! X O   . O X ! . ! O .   O X X . O
	 * O X X X X X O   # # # # # # # #   O O O O O */

	/* Extract the other two liberties. */
	coord_t other_libs[2];
	bool other_libs_adj[2];
	for (int i = 0, j = 0; i < 3; i++) {
		coord_t lib = board_group_info(b, g).lib[i];
		if (lib != to) {
			other_libs_adj[j] = coord_is_adjecent(lib, to, b);
			other_libs[j++] = lib;
		}
	}

	/* Make sure this move is not useful by gaining liberties,
	 * splitting the other two liberties (quite possibly splitting
	 * 3-eyespace!) or connecting to a different group. */
	if (immediate_liberty_count(b, to) - (other_libs_adj[0] || other_libs_adj[1]) > 0)
		return false;
	assert(!(other_libs_adj[0] && other_libs_adj[1]));
	if (s->groupcts[color] > 1)
		return false;

	/* Playing on the third liberty might be useful if it enables
	 * capturing some group. */
	for (int i = 0; i < s->groupcts[stone_other(color)]; i++)
		if (board_group_info(b, s->groupids[stone_other(color)][i]).libs <= 2)
			return false;


	/* Okay. This looks like a pretty dangerous situation. The
	 * move looks useless, it definitely converts us to a 2-lib
	 * group. But we still want to play it e.g. if it takes off
	 * liberties of some unconspicous enemy group, and of course
	 * also at the game end to leave just single-point eyes. */

	if (DEBUGL(6))
		fprintf(stderr, "3-lib danger\n");

	/* Therefore, the final suicidal test is: (After filling this
	 * liberty,) when opponent fills liberty [0], playing liberty
	 * [1] will not help the group, or vice versa. */
	bool other_libs_neighbors = coord_is_adjecent(other_libs[0], other_libs[1], b);
	for (int i = 0; i < 2; i++) {
		int null_libs = other_libs_neighbors + other_libs_adj[i];
		if (board_is_one_point_eye(b, other_libs[1 - i], color)) {
			/* The other liberty is an eye, happily go ahead.
			 * There are of course situations where this will
			 * take off semeai liberties, but without this check,
			 * many terminal endgame plays will be messed up. */
			return false;
		}
		if (immediate_liberty_count(b, other_libs[i]) - null_libs > 1) {
			/* Gains liberties. */
			/* TODO: Check for ladder! */
next_lib:
			continue;
		}
		foreach_neighbor(b, other_libs[i], {
			if (board_at(b, c) == color
			    && group_at(b, c) != g
			    && board_group_info(b, group_at(b, c)).libs > 1) {
				/* Can connect to a friend. */
				/* TODO: > 2? But maybe the group can capture
				 * a neighbor! But then better let it do that
				 * first? */
				goto next_lib;
			}
		});
		/* If we can capture a neighbor, better do it now
		 * before wasting a liberty. So no need to check. */
		/* Ok, the last liberty has no way to get out. */
		if (DEBUGL(6))
			fprintf(stderr, "3-lib dangerous: %s\n", coord2sstr(other_libs[i], b));
		return true;
	}

	return false;
}

static int
examine_friendly_groups(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
	for (int i = 0; i < s->groupcts[color]; i++) {
		/* We can escape by connecting to this group if it's
		 * not in atari. */
		group_t g = s->groupids[color][i];

		if (board_group_info(b, g).libs == 1) {
			if (!s->needs_more_lib)
				s->friend_has_no_libs = true;
			// or we already have a friend with 1 lib
			continue;
		}

		/* Could we self-atari the group here? */
		if (board_group_info(b, g).libs > 2) {
			if (board_group_info(b, g).libs == 3
			    && three_liberty_suicide(b, g, color, to, s))
				return true;
			return false;
		}

		/* We need to have another liberty, and
		 * it must not be the other liberty of
		 * the group. */
		int lib2 = board_group_other_lib(b, g, to);
		/* Maybe we already looked at another
		 * group providing one liberty? */
		if (s->needs_more_lib && s->needs_more_lib != g
		    && s->needs_more_lib_except != lib2)
			return false;

		/* Can we get the liberty locally? */
		/* Yes if we are route to more liberties... */
		if (s->groupcts[S_NONE] > 1)
			return false;
		/* ...or one liberty, but not lib2. */
		if (s->groupcts[S_NONE] > 0
		    && !coord_is_adjecent(lib2, to, b))
			return false;

		/* ...ok, then we can still contribute a liberty
		 * later by capturing something. */
		s->needs_more_lib = g;
		s->needs_more_lib_except = lib2;
		s->friend_has_no_libs = false;
	}

	return -1;
}

static int
examine_enemy_groups(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
	/* We may be able to gain a liberty by capturing this group. */
	group_t can_capture = 0;

	/* Examine enemy groups: */
	for (int i = 0; i < s->groupcts[stone_other(color)]; i++) {
		/* We can escape by capturing this group if it's in atari. */
		group_t g = s->groupids[stone_other(color)][i];
		if (board_group_info(b, g).libs > 1)
			continue;

		/* But we need to get to at least two liberties by this;
		 * we already have one outside liberty, or the group is
		 * more than 1 stone (in that case, capturing is always
		 * nice!). */
		if (s->groupcts[S_NONE] > 0 || !group_is_onestone(b, g))
			return false;
		/* ...or, it's a ko stone, */
		if (neighbor_count_at(b, g, color) + neighbor_count_at(b, g, S_OFFBOARD) == 3) {
			/* and we don't have a group to save: then, just taking
			 * single stone means snapback! */
			if (!s->friend_has_no_libs)
				return false;
		}
		/* ...or, we already have one indirect liberty provided
		 * by another group. */
		if (s->needs_more_lib || (can_capture && can_capture != g))
			return false;
		can_capture = g;

	}

	if (DEBUGL(6))
		fprintf(stderr, "no cap group\n");

	if (!s->needs_more_lib && !can_capture && !s->groupcts[S_NONE]) {
		/* We have no hope for more fancy tactics - this move is simply
		 * a suicide, not even a self-atari. */
		if (DEBUGL(6))
			fprintf(stderr, "suicide\n");
		return true;
	}
	/* XXX: I wonder if it makes sense to continue if we actually
	 * just !s->needs_more_lib. */

	return -1;
}

static int
setup_nakade_or_snapback(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
	/* There is another possibility - we can self-atari if it is
	 * a nakade: we put an enemy group in atari from the inside. */
	/* This branch also allows eyes falsification:
	 * O O O . .  (This is different from throw-in to false eye
	 * X X O O .  checked below in that there is no X stone at the
	 * X . X O .  right of the star point in this diagram.)
	 * X X X O O
	 * X O * . . */
	/* TODO: Allow to only nakade if the created shape is dead
	 * (http://senseis.xmp.net/?Nakade). */

	/* This branch also covers snapback, which is kind of special
	 * nakade case. ;-) */
	for (int i = 0; i < s->groupcts[stone_other(color)]; i++) {
		group_t g = s->groupids[stone_other(color)][i];
		if (board_group_info(b, g).libs != 2)
			goto next_group;

		/* We must make sure the other liberty of that group:
		 * (i) is an internal liberty
		 * (ii) filling it to capture our group will not gain
		 * safety */

		/* Let's look at neighbors of the other liberty: */
		int lib2 = board_group_other_lib(b, g, to);
		foreach_neighbor(b, lib2, {
			/* This neighbor of course does not contribute
			 * anything to the enemy. */
			if (board_at(b, c) == S_OFFBOARD)
				continue;

			/* If the other liberty has empty neighbor,
			 * it must be the original liberty; otherwise,
			 * since the whole group has only 2 liberties,
			 * the other liberty may not be internal and
			 * we are nakade'ing eyeless group from outside,
			 * which is stupid. */
			if (board_at(b, c) == S_NONE) {
				if (c == to)
					continue;
				else
					goto next_group;
			}

			int g2 = group_at(b, c);
			/* If the neighbor is of our color, it must
			 * be also a 2-lib group. If it is more,
			 * we CERTAINLY want that liberty to be played
			 * first, what if it is an alive group? If it
			 * is in atari, we want to extend from it to
			 * prevent eye-making capture. However, if it
			 * is 2-lib, it is selfatari connecting two
			 * nakade'ing groups! */
			/* X X X X  We will not allow play on 'a',
			 * X X a X  because 'b' would capture two
			 * X O b X  different groups, forming two
			 * X X X X  eyes. */
			if (board_at(b, c) == color) {
				if (board_group_info(b, group_at(b, c)).libs == 2)
					continue;
				goto next_group;
			}

			/* The neighbor is enemy color. It's ok if
			 * it's still the same group or this is its
			 * only liberty. */
			if (g == g2 || board_group_info(b, g2).libs == 1)
				continue;
			/* Otherwise, it must have the exact same
			 * liberties as the original enemy group. */
			if (board_group_info(b, g2).libs == 2
			    && (board_group_info(b, g2).lib[0] == to
			        || board_group_info(b, g2).lib[1] == to))
				continue;

			goto next_group;
		});

		/* Now, we must distinguish between nakade and eye
		 * falsification; we must not falsify an eye by more
		 * than two stones. */
		if (s->groupcts[color] < 1)
			return false; // simple throw-in
		if (s->groupcts[color] == 1 && group_is_onestone(b, s->groupids[color][0])) {
			/* More complex throw-in - we are in one of
			 * three situations:
			 * a O O O O X  b O O O X  c O O O X
			 *   O . X . O    O X . .    O . X .
			 *   # # # # #    # # # #    # # # #
			 * b is desirable here (since maybe O has no
			 * backup two eyes); a may be desirable, but
			 * is tested next in check_throwin(). c is
			 * never desirable. */
			group_t g2 = s->groupids[color][0];
			assert(board_group_info(b, g2).libs <= 2);
			if (board_group_info(b, g2).libs == 1)
				return false; // b
			goto next_group; // a or c
		}

		/* We would create more than 2-stone group; in that
		 * case, the liberty of our result must be lib2,
		 * indicating this really is a nakade. */
		for (int j = 0; j < s->groupcts[color]; j++) {
			group_t g2 = s->groupids[color][j];
			assert(board_group_info(b, g2).libs <= 2);
			if (board_group_info(b, g2).libs == 2) {
				if (board_group_info(b, g2).lib[0] != lib2
				    && board_group_info(b, g2).lib[1] != lib2)
					goto next_group;
			} else {
				assert(board_group_info(b, g2).lib[0] == to);
			}
		}

		return false;
next_group:	
		/* Unless we are dealing with snapback setup, we don't need to look
		 * further. */
		if (s->groupcts[color])
			return -1;
	}

	return -1;
}

static int
check_throwin(struct board *b, enum stone color, coord_t to, struct selfatari_state *s)
{
	/* We can be throwing-in to false eye:
	 * X X X O X X X O X X X X X
	 * X . * X * O . X * O O . X
	 * # # # # # # # # # # # # # */
	/* We cannot sensibly throw-in into a corner. */
	if (neighbor_count_at(b, to, S_OFFBOARD) < 2
	    && neighbor_count_at(b, to, stone_other(color))
	       + neighbor_count_at(b, to, S_OFFBOARD) == 3
	    && board_is_false_eyelike(b, to, stone_other(color))) {
		assert(s->groupcts[color] <= 1);
		/* Single-stone throw-in may be ok... */
		if (s->groupcts[color] == 0) {
			/* O X .  There is one problem - when it's
			 * . * X  actually not a throw-in!
			 * # # #  */
			foreach_neighbor(b, to, {
				if (board_at(b, c) == S_NONE) {
					/* Is the empty neighbor an escape path? */
					/* (Note that one S_NONE neighbor is already @to.) */
					if (neighbor_count_at(b, c, stone_other(color))
					    + neighbor_count_at(b, c, S_OFFBOARD) < 2)
						return -1;
				}
			});
			return false;
		}

		/* Multi-stone throwin...? */
		assert(s->groupcts[color] == 1);
		group_t g = s->groupids[color][0];

		assert(board_group_info(b, g).libs <= 2);
		/* Suicide is definitely NOT ok, no matter what else
		 * we could test. */
		if (board_group_info(b, g).libs == 1)
			return true;

		/* In that case, we must be connected to at most one stone,
		 * or throwin will not destroy any eyes. */
		if (group_is_onestone(b, g))
			return false;
	}
	return -1;
}

bool
is_bad_selfatari_slow(struct board *b, enum stone color, coord_t to)
{
	if (DEBUGL(5))
		fprintf(stderr, "sar check %s %s\n", stone2str(color), coord2sstr(to, b));
	/* Assess if we actually gain any liberties by this escape route.
	 * Note that this is not 100% as we cannot check whether we are
	 * connecting out or just to ourselves. */

	struct selfatari_state s;
	memset(&s, 0, sizeof(s));
	int d;

	foreach_neighbor(b, to, {
		enum stone color = board_at(b, c);
		group_t group = group_at(b, c);
		bool dup = false;
		for (int i = 0; i < s.groupcts[color]; i++)
			if (s.groupids[color][i] == group) {
				dup = true;
				break;
			}
		if (!dup) {
			s.groupids[color][s.groupcts[color]++] = group_at(b, c);
		}
	});

	/* We have shortage of liberties; that's the point. */
	assert(s.groupcts[S_NONE] <= 1);

	d = examine_friendly_groups(b, color, to, &s);
	if (d >= 0)
		return d;

	if (DEBUGL(6))
		fprintf(stderr, "no friendly group\n");

	d = examine_enemy_groups(b, color, to, &s);
	if (d >= 0)
		return d;

	if (DEBUGL(6))
		fprintf(stderr, "no escape\n");

	d = setup_nakade_or_snapback(b, color, to, &s);
	if (d >= 0)
		return d;

	if (DEBUGL(6))
		fprintf(stderr, "no nakade group\n");

	d = check_throwin(b, color, to, &s);
	if (d >= 0)
		return d;

	if (DEBUGL(6))
		fprintf(stderr, "no throw-in group\n");

	/* No way to pull out, no way to connect out. This really
	 * is a bad self-atari! */
	return true;
}


coord_t
selfatari_cousin(struct board *b, enum stone color, coord_t coord)
{
	group_t groups[4]; int groups_n = 0;
	foreach_neighbor(b, coord, {
		enum stone s = board_at(b, c);
		if (s != color) continue;
		group_t g = group_at(b, c);
		if (board_group_info(b, g).libs == 2)
			groups[groups_n++] = g;
	});

	if (!groups_n)
		return pass;
	group_t group = groups[fast_random(groups_n)];

	coord_t lib2 = board_group_other_lib(b, group, coord);
	if (is_bad_selfatari(b, color, lib2))
		return pass;
	return lib2;
}
