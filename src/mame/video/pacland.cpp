// license:BSD-3-Clause
// copyright-holders:Ernesto Corvi
/***************************************************************************

Sprite/tile priority is quite complex in this game: it is handled both
internally to the CUS29 chip, and externally to it.

The bg tilemap is always behind everything.

The CUS29 mixes two 8-bit inputs, one from sprites and one from the fg
tilemap. 0xff is the transparent color. CUS29 also takes a PRI input, telling
which of the two color inputs has priority. Additionally, sprite pixels of
color >= 0xf0 always have priority.
The priority bit comes from the tilemap RAM, but through an additional filter:
sprite pixels of color < 0x80 act as a "cookie cut" mask, handled externally,
which overload the PRI bit, making the sprite always have priority. The external
RAM that holds this mask contains the OR of all sprite pixels drawn at a certain
position, therefore when sprites overlap, it is sufficient for one of them to
have color < 0x80 to promote priority of the frontmost sprite. This is used
to draw the light in round 19.

The CUS29 outputs an 8-bit pixel color, but only the bottom 7 bits are externally
checked to determine whether it is transparent or not; therefore, both 0xff and
0x7f are transparent. This is again used to draw the light in round 19, because
sprite color 0x7f will erase the tilemap and force it to be transparent.

***************************************************************************/

#include "emu.h"
#include "includes/pacland.h"


/***************************************************************************

  Convert the color PROMs.

  Pacland has one 1024x8 and one 1024x4 palette PROM; and three 1024x8 lookup
  table PROMs (sprites, bg tiles, fg tiles).
  The palette has 1024 colors, but it is bank switched (4 banks) and only 256
  colors are visible at a time. So, instead of creating a static palette, we
  modify it when the bank switching takes place.
  The color PROMs are connected to the RGB output this way:

  bit 7 -- 220 ohm resistor  -- GREEN
        -- 470 ohm resistor  -- GREEN
        -- 1  kohm resistor  -- GREEN
        -- 2.2kohm resistor  -- GREEN
        -- 220 ohm resistor  -- RED
        -- 470 ohm resistor  -- RED
        -- 1  kohm resistor  -- RED
  bit 0 -- 2.2kohm resistor  -- RED

  bit 3 -- 220 ohm resistor  -- BLUE
        -- 470 ohm resistor  -- BLUE
        -- 1  kohm resistor  -- BLUE
  bit 0 -- 2.2kohm resistor  -- BLUE

***************************************************************************/

void pacland_state::switch_palette()
{
	const uint8_t *color_prom = m_color_prom + 256 * m_palette_bank;

	for (int i = 0; i < 256; i++)
	{
		int bit0,bit1,bit2,bit3;

		bit0 = BIT(color_prom[0], 0);
		bit1 = BIT(color_prom[0], 1);
		bit2 = BIT(color_prom[0], 2);
		bit3 = BIT(color_prom[0], 3);
		int const r = 0x0e * bit0 + 0x1f * bit1 + 0x43 * bit2 + 0x8f * bit3;

		bit0 = BIT(color_prom[0], 4);
		bit1 = BIT(color_prom[0], 5);
		bit2 = BIT(color_prom[0], 6);
		bit3 = BIT(color_prom[0], 7);
		int const g = 0x0e * bit0 + 0x1f * bit1 + 0x43 * bit2 + 0x8f * bit3;

		bit0 = BIT(color_prom[1024], 0);
		bit1 = BIT(color_prom[1024], 1);
		bit2 = BIT(color_prom[1024], 2);
		bit3 = BIT(color_prom[1024], 3);
		int const b = 0x0e * bit0 + 0x1f * bit1 + 0x43 * bit2 + 0x8f * bit3;

		color_prom++;

		m_palette->set_indirect_color(i, rgb_t(r, g, b));
	}
}

void pacland_state::pacland_palette(palette_device &palette)
{
	uint8_t const *color_prom = &m_color_prom[0];

	// skip the palette data, it will be initialized later
	color_prom += 2 * 0x400;
	// color_prom now points to the beginning of the lookup table

	for (int i = 0; i < 0x400; i++)
		palette.set_pen_indirect(m_gfxdecode->gfx(0)->colorbase() + i, *color_prom++);

	// Background
	for (int i = 0; i < 0x400; i++)
		palette.set_pen_indirect(m_gfxdecode->gfx(1)->colorbase() + i, *color_prom++);

	// Sprites
	for (int i = 0; i < 0x400; i++)
		palette.set_pen_indirect(m_gfxdecode->gfx(2)->colorbase() + i, *color_prom++);

	m_palette_bank = 0;
	switch_palette();

	// precalculate transparency masks for sprites
	m_transmask[0] = std::make_unique<uint32_t[]>(64);
	m_transmask[1] = std::make_unique<uint32_t[]>(64);
	m_transmask[2] = std::make_unique<uint32_t[]>(64);
	for (int i = 0; i < 64; i++)
	{
		// start with no transparency
		m_transmask[0][i] = m_transmask[1][i] = m_transmask[2][i] = 0;

		// iterate over all palette entries except the last one
		for (int palentry = 0; palentry < 0x100; palentry++)
		{
			uint32_t const mask = palette.transpen_mask(*m_gfxdecode->gfx(2), i, palentry);

			/* transmask[0] is a mask that is used to draw only high priority sprite pixels; thus, pens
			   $00-$7F are opaque, and others are transparent */
			if (palentry >= 0x80)
				m_transmask[0][i] |= mask;

			/* transmask[1] is a normal drawing masking with palette entries $7F and $FF transparent */
			if ((palentry & 0x7f) == 0x7f)
				m_transmask[1][i] |= mask;

			/* transmask[2] is a mask of the topmost priority sprite pixels; thus pens $F0-$FE are
			   opaque, and others are transparent */
			if (palentry < 0xf0 || palentry == 0xff)
				m_transmask[2][i] |= mask;
		}
	}
}



/***************************************************************************

  Callbacks for the TileMap code

***************************************************************************/

TILE_GET_INFO_MEMBER(pacland_state::get_bg_tile_info)
{
	int offs = tile_index * 2;
	int attr = m_videoram2[offs + 1];
	int code = m_videoram2[offs] + ((attr & 0x01) << 8);
	int color = ((attr & 0x3e) >> 1) + ((code & 0x1c0) >> 1);
	int flags = TILE_FLIPYX(attr >> 6);

	tileinfo.set(1, code, color, flags);
}

TILE_GET_INFO_MEMBER(pacland_state::get_fg_tile_info)
{
	int offs = tile_index * 2;
	int attr = m_videoram[offs + 1];
	int code = m_videoram[offs] + ((attr & 0x01) << 8);
	int color = ((attr & 0x1e) >> 1) + ((code & 0x1e0) >> 1);
	int flags = TILE_FLIPYX(attr >> 6);

	tileinfo.category = (attr & 0x20) ? 1 : 0;
	tileinfo.group = color;

	tileinfo.set(0, code, color, flags);
}



/***************************************************************************

  Start the video hardware emulation.

***************************************************************************/

void pacland_state::video_start()
{
	m_screen->register_screen_bitmap(m_sprite_bitmap);

	m_screen->register_screen_bitmap(m_fg_bitmap);
	m_fg_bitmap.fill(0xffff);

	m_bg_tilemap = &machine().tilemap().create(*m_gfxdecode, tilemap_get_info_delegate(*this, FUNC(pacland_state::get_bg_tile_info)), TILEMAP_SCAN_ROWS, 8, 8, 64, 32);
	m_fg_tilemap = &machine().tilemap().create(*m_gfxdecode, tilemap_get_info_delegate(*this, FUNC(pacland_state::get_fg_tile_info)), TILEMAP_SCAN_ROWS, 8, 8, 64, 32);

	m_bg_tilemap->set_scrolldx(3, 340);
	m_fg_tilemap->set_scrolldx(0, 336); /* scrolling portion needs an additional offset when flipped */
	m_fg_tilemap->set_scroll_rows(32);

	/* create one group per color code; for each group, set the transparency mask
	   to correspond to the pens that are 0x7f or 0xff */
	assert(m_gfxdecode->gfx(0)->colors() <= TILEMAP_NUM_GROUPS);
	for (int color = 0; color < m_gfxdecode->gfx(0)->colors(); color++)
	{
		uint32_t mask = m_palette->transpen_mask(*m_gfxdecode->gfx(0), color, 0x7f);
		mask |= m_palette->transpen_mask(*m_gfxdecode->gfx(0), color, 0xff);
		m_fg_tilemap->set_transmask(color, mask, 0);
	}

	membank("bank1")->configure_entries(0, 8, memregion("maincpu")->base() + 0x10000, 0x2000);

	save_item(NAME(m_palette_bank));
	save_item(NAME(m_scroll0));
	save_item(NAME(m_scroll1));
}



/***************************************************************************

  Memory handlers

***************************************************************************/

void pacland_state::videoram_w(offs_t offset, uint8_t data)
{
	m_videoram[offset] = data;
	m_fg_tilemap->mark_tile_dirty(offset / 2);
}

void pacland_state::videoram2_w(offs_t offset, uint8_t data)
{
	m_videoram2[offset] = data;
	m_bg_tilemap->mark_tile_dirty(offset / 2);
}

void pacland_state::scroll0_w(offs_t offset, uint8_t data)
{
	m_scroll0 = data + 256 * offset;
}

void pacland_state::scroll1_w(offs_t offset, uint8_t data)
{
	m_scroll1 = data + 256 * offset;
}

void pacland_state::bankswitch_w(uint8_t data)
{
	membank("bank1")->set_entry(data & 0x07);

//  pbc = data & 0x20;

	if (m_palette_bank != ((data & 0x18) >> 3))
	{
		m_palette_bank = (data & 0x18) >> 3;
		switch_palette();
	}
}



/***************************************************************************

  Display refresh

***************************************************************************/

/* the sprite generator IC is the same as Mappy */
void pacland_state::draw_sprites(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect, int flip, int whichmask)
{
	uint8_t *spriteram = m_spriteram + 0x780;
	uint8_t *spriteram_2 = spriteram + 0x800;
	uint8_t *spriteram_3 = spriteram_2 + 0x800;

	for (int offs = 0;offs < 0x80;offs += 2)
	{
		static const int gfx_offs[2][2] =
		{
			{ 0, 1 },
			{ 2, 3 }
		};
		int sprite = spriteram[offs] + ((spriteram_3[offs] & 0x80) << 1);
		int color = spriteram[offs+1] & 0x3f;
		int sx = (spriteram_2[offs+1]) + 0x100*(spriteram_3[offs+1] & 1) - 47;
		int sy = 256 - spriteram_2[offs] + 9;
		int flipx = (spriteram_3[offs] & 0x01);
		int flipy = (spriteram_3[offs] & 0x02) >> 1;
		int sizex = (spriteram_3[offs] & 0x04) >> 2;
		int sizey = (spriteram_3[offs] & 0x08) >> 3;
		int x,y;

		sprite &= ~sizex;
		sprite &= ~(sizey << 1);

		if (flip)
		{
			flipx ^= 1;
			flipy ^= 1;
		}

		sy -= 16 * (sizey + 1); // sprites could not be displayed at the bottom of the screen
		sy = (sy & 0xff) - 16; // fix wraparound

		for (y = 0;y <= sizey;y++)
		{
			for (x = 0;x <= sizex;x++)
			{
				if (whichmask != 0)
					m_gfxdecode->gfx(2)->transmask(bitmap,cliprect,
						sprite + gfx_offs[y ^ (sizey * flipy)][x ^ (sizex * flipx)],
						color,
						flipx,flipy,
						sx + 16*x,sy + 16*y,m_transmask[whichmask][color]);
				else
					m_gfxdecode->gfx(2)->prio_transmask(bitmap,cliprect,
						sprite + gfx_offs[y ^ (sizey * flipy)][x ^ (sizex * flipx)],
						color,
						flipx,flipy,
						sx + 16*x,sy + 16*y,
						screen.priority(),0,m_transmask[whichmask][color]);
			}
		}
	}
}


void pacland_state::draw_fg(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect, int priority )
{
	/* draw tilemap transparently over it; this will leave invalid pens (0xffff)
	   anywhere where the fg_tilemap should be transparent; note that we assume
	   the fg_bitmap has been pre-erased to 0xffff */
	m_fg_tilemap->draw(screen, m_fg_bitmap, cliprect, priority, 0);

	/* now copy the fg_bitmap to the destination wherever the sprite pixel allows */
	for (int y = cliprect.min_y; y <= cliprect.max_y; y++)
	{
		uint8_t const *const pri = &screen.priority().pix(y);
		uint16_t *const src = &m_fg_bitmap.pix(y);
		uint16_t *const dst = &bitmap.pix(y);

		/* only copy if the priority bitmap is 0 (no high priority sprite) and the
		   source pixel is not the invalid pen; also clear to 0xffff when finished */
		for (int x = cliprect.min_x; x <= cliprect.max_x; x++)
		{
			uint16_t pix = src[x];
			if (pix != 0xffff)
			{
				src[x] = 0xffff;
				if (pri[x] == 0)
					dst[x] = pix;
			}
		}
	}
}


uint32_t pacland_state::screen_update(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	int flip = flip_screen();

	for (int row = 5; row < 29; row++)
		m_fg_tilemap->set_scrollx(row, m_scroll0 - (flip ? 7 : 0));
	m_bg_tilemap->set_scrollx(0, m_scroll1);

	/* draw high priority sprite pixels, setting priority bitmap to non-zero
	   wherever there is a high-priority pixel; note that we draw to the bitmap
	   which is safe because the bg_tilemap draw will overwrite everything */
	screen.priority().fill(0x00, cliprect);
	draw_sprites(screen, bitmap, cliprect, flip, 0);

	/* draw background */
	m_bg_tilemap->draw(screen, bitmap, cliprect, 0, 0);

	/* draw low priority fg tiles */
	draw_fg(screen, bitmap, cliprect, 0);

	/* draw sprites with regular transparency */
	draw_sprites(screen, bitmap, cliprect, flip, 1);

	/* draw sprite pixels in a temporary bitmap with colortable values >= 0xf0 */
	m_sprite_bitmap.fill(0, cliprect);
	draw_sprites(screen, m_sprite_bitmap, cliprect, flip, 2);
	for (int y = cliprect.min_y; y <= cliprect.max_y; y++)
	{
		uint16_t *const spr = &m_sprite_bitmap.pix(y);
		uint16_t const *const bmp = &bitmap.pix(y);
		for (int x = cliprect.min_x; x <= cliprect.max_x; x++)
		{
			/* clear to 0 if "m_sprite_bitmap" and "bitmap" are different,
			   because not redraw pixels that are not visible in "bitmap"
			   in this way, keep sprite-sprite priorities intact */
			if (spr[x] != 0 && spr[x] != bmp[x])
				spr[x] = 0;
		}
	}

	/* draw high priority fg tiles */
	draw_fg(screen, bitmap, cliprect, 1);

	/* draw sprite pixels with colortable values >= 0xf0, which have priority over everything */
	copybitmap_trans(bitmap, m_sprite_bitmap, 0, 0, 0, 0, cliprect, 0);

	return 0;
}
