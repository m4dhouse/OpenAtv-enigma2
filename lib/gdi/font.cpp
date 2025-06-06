#include <lib/gdi/font.h>

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#include <byteswap.h>

#ifndef BYTE_ORDER
#error "no BYTE_ORDER defined!"
#endif

// use this for init Freetype...
#include <ft2build.h>
#include FT_FREETYPE_H
#define FTC_Image_Cache_New(a,b)	FTC_ImageCache_New(a,b)
#define FTC_SBit_Cache_New(a,b)		FTC_SBitCache_New(a,b)
#define FTC_SBit_Cache_Lookup(a,b,c,d)	FTC_SBitCache_Lookup(a,b,c,d,NULL)

#include <lib/base/eerror.h>
#include <lib/gdi/lcd.h>
#include <lib/gdi/grc.h>
#include <lib/base/elock.h>
#include <lib/base/init.h>
#include <lib/base/init_num.h>

#include <fribidi/fribidi.h>

#include <map>

fontRenderClass *fontRenderClass::instance;

static pthread_mutex_t ftlock=PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;

struct fntColorCacheKey
{
	gRGB start, end;
	fntColorCacheKey(const gRGB &start, const gRGB &end)
		: start(start), end(end)
	{
	}
	bool operator <(const fntColorCacheKey &c) const
	{
		if (start < c.start)
			return 1;
		else if (start == c.start)
			return end < c.end;
		return 0;
	}
};

std::map<fntColorCacheKey,gLookup> colorcache;

static gLookup &getColor(const gPalette &pal, const gRGB &start, const gRGB &end)
{
	fntColorCacheKey key(start, end);
	std::map<fntColorCacheKey,gLookup>::iterator i=colorcache.find(key);
	if (i != colorcache.end())
		return i->second;
	gLookup &n=colorcache.insert(std::pair<fntColorCacheKey,gLookup>(key,gLookup())).first->second;
//	eDebug("[Font] Creating new font color cache entry %02x%02x%02x%02x .. %02x%02x%02x%02x", start.a, start.r, start.g, start.b,
//		end.a, end.r, end.g, end.b);
	n.build(16, pal, start, end);
//	eDebugNoNewLineStart("[Font] ");
//	for (int i=0; i<16; i++)
//		eDebugNoNewLine("%02x|%02x%02x%02x%02x ", (int)n.lookup[i], pal.data[n.lookup[i]].a, pal.data[n.lookup[i]].r, pal.data[n.lookup[i]].g, pal.data[n.lookup[i]].b);
//	eDebugNoNewLine("\n");
	return n;
}

fontRenderClass *fontRenderClass::getInstance()
{
	return instance;
}

FT_Error myFTC_Face_Requester(	FTC_FaceID	face_id,
				FT_Library	library,
				FT_Pointer	request_data,
				FT_Face*	aface)
{
	return ((fontRenderClass*)request_data)->FTC_Face_Requester(face_id, aface);
}


FT_Error fontRenderClass::FTC_Face_Requester(FTC_FaceID	face_id, FT_Face* aface)
{
	fontListEntry *font=(fontListEntry *)face_id;
	if (!font)
		return -1;

//	eDebug("[Font] FTC_Face_Requester (%s)", font->face.c_str());

	int error;
	if ((error=FT_New_Face(library, font->filename.c_str(), 0, aface)))
	{
		eDebug("[Font] Failed: %s", strerror(error));
		return error;
	}
	FT_Select_Charmap(*aface, ft_encoding_unicode);
	return 0;
}

int fontRenderClass::getFaceProperties(const std::string &face, FTC_FaceID &id, int &renderflags)
{
	for (fontListEntry *f=font; f; f=f->next)
	{
		if (f->face == face)
		{
			id = (FTC_FaceID)f;
			renderflags = f->renderflags;
			return 0;
		}
	}
	return -1;
}

inline FT_Error fontRenderClass::getGlyphBitmap(FTC_Image_Desc *font, FT_UInt glyph_index, FTC_SBit *sbit)
{
	return FTC_SBit_Cache_Lookup(sbitsCache, font, glyph_index, sbit);
}

inline FT_Error fontRenderClass::getGlyphImage(FTC_Image_Desc *font, FT_UInt glyph_index, FT_Glyph *glyph, FT_Glyph *borderglyph, int bordersize)
{
	FT_Glyph image;
	FT_Error err = FTC_ImageCache_Lookup(imageCache, font, glyph_index, &image, NULL);
	if (err) return err;

	if (glyph)
	{
		err = FT_Glyph_Copy(image, glyph);
		if (err) return err;
	}

	if (borderglyph && bordersize)
	{
		err = FT_Glyph_Copy(image, borderglyph);
		if (err) return err;
		if (bordersize != strokerRadius)
		{
			strokerRadius = bordersize;
			FT_Stroker_Set(stroker, strokerRadius, FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
		}
		err = FT_Glyph_Stroke(borderglyph, stroker, 1);
	}
	return err;
}

std::string fontRenderClass::AddFont(const std::string &filename, const std::string &name, int scale, int renderflags)
{
	eDebugNoNewLineStart("[Font] Adding font '%s'", filename.c_str());
	fflush(stdout);
	int error;
	FT_Face face;

	singleLock s(ftlock);
	if ((error=FT_New_Face(library, filename.c_str(), 0, &face)))
	{
		eWarning("[Font] Failed: %m");
		return std::string();
	}
	FT_Done_Face(face);

	fontListEntry *n = new fontListEntry;
	n->filename = filename;
	n->face = name;
	n->scale = scale;
	n->renderflags = renderflags;
	n->next=font;
	font=n;

	eDebugNoNewLine(" -> '%s'.\n", n->face.c_str());

	return n->face;
}

fontRenderClass::fontListEntry::~fontListEntry()
{
}

fontRenderClass::fontRenderClass(): fb(fbClass::getInstance())
{
	instance=this;
	eDebug("[Font] Initializing lib.");
	{
		if (FT_Init_FreeType(&library))
		{
			eDebug("[Font] Initializing failed!");
			return;
		}
	}
	eDebug("[Font] Loading fonts.");
	fflush(stdout);
	font=0;

	int maxbytes=4*1024*1024;
	eDebug("[Font] Intializing font cache, using max. %dMB.", maxbytes/1024/1024);
	fflush(stdout);
	{
		if (FTC_Manager_New(library, 8, 8, maxbytes, myFTC_Face_Requester, this, &cacheManager))
		{
			eDebug("[Font] Initializing font cache failed!");
			return;
		}
		if (!cacheManager)
		{
			eDebug("[Font] Initializing font cache manager error!");
			return;
		}
		if (FTC_SBit_Cache_New(cacheManager, &sbitsCache))
		{
			eDebug("[Font] Initializing font cache sbit failed!");
			return;
		}
		if (FTC_Image_Cache_New(cacheManager, &imageCache))
		{
			eDebug("[Font] Initializing font cache imagecache failed!");
		}
		if (FT_Stroker_New(library, &stroker))
		{
			eDebug("[Font] Initializing font stroker failed!");
		}
	}
	strokerRadius = -1;
	return;
}

float fontRenderClass::getLineHeight(const gFont& font)
{
	if (!instance)
		return 0;
	ePtr<Font> fnt;
	getFont(fnt, font.family.c_str(), font.pointSize);
	if (!fnt)
		return 0;
	singleLock s(ftlock);
	FT_Face current_face;
	if ((FTC_Manager_LookupFace(cacheManager, fnt->scaler.face_id, &current_face) < 0) ||
	    (FTC_Manager_LookupSize(cacheManager, &fnt->scaler, &fnt->size) < 0))
	{
		eDebug("[Font] FTC_Manager_Lookup_Size failed!");
		return 0;
	}
	int height = current_face->size->metrics.height;
	if (!height)
	{
		/* some fonts don't have height filled in. Estimate it based on the bbox dimensions. */
		/* Usually, 'height' is less than the complete boundingbox height, so we use only yMax, to avoid getting a much too large line spacing */
		height = FT_MulFix(current_face->bbox.yMax, current_face->size->metrics.y_scale);
	}
	return (height>>6);
}

fontRenderClass::~fontRenderClass()
{
	singleLock s(ftlock);
	while(font)
	{
		fontListEntry *f=font;
		font=font->next;
		delete f;
	}
//	auskommentiert weil freetype und enigma die kritische masse des suckens ueberschreiten.
//	FTC_Manager_Done(cacheManager);
//	FT_Done_FreeType(library);
}

int fontRenderClass::getFont(ePtr<Font> &font, const std::string &face, int size, int tabwidth)
{
	FTC_FaceID id;
	int renderflags;
	if (getFaceProperties(face, id, renderflags) < 0)
	{
		font = 0;
		return -1;
	}
	font = new Font(this, id, size * ((fontListEntry*)id)->scale / 100, tabwidth, renderflags);
	return 0;
}

// get all font faces (names) available in enigma2
std::vector<std::string> fontRenderClass::getFontFaces()
{
	std::vector<std::string> v;
	for (fontListEntry *f=font; f; f=f->next)
	{
		v.push_back(f->face);
	}
	return v;
}

void addFont(const char *filename, const char *alias, int scale_factor, int is_replacement, int renderflags)
{
	fontRenderClass::getInstance()->AddFont(filename, alias, scale_factor, renderflags);
	if (is_replacement == 1)
		eTextPara::setReplacementFont(alias);
	else if (is_replacement == -1)
		eTextPara::setFallbackFont(alias);

}

DEFINE_REF(Font);

Font::Font(fontRenderClass *render, FTC_FaceID faceid, int isize, int tw, int renderflags): tabwidth(tw)
{
	renderer=render;
	font.face_id = faceid;
	font.width = isize;
	font.height = isize;
	font.flags = renderflags;
	scaler.face_id = faceid;
	scaler.width = isize;
	scaler.height = isize;
	scaler.pixel = 1;
	height=isize;
	if (tabwidth==-1)
		tabwidth=8*isize;
//	font.image_type |= ftc_image_flag_autohinted;
}

inline FT_Error Font::getGlyphBitmap(FT_UInt glyph_index, FTC_SBit *sbit)
{
	return renderer->getGlyphBitmap(&font, glyph_index, sbit);
}

inline FT_Error Font::getGlyphImage(FT_UInt glyph_index, FT_Glyph *glyph, FT_Glyph *borderglyph, int bordersize)
{
	return renderer->getGlyphImage(&font, glyph_index, glyph, borderglyph, bordersize);
}

Font::~Font()
{
}

DEFINE_REF(eTextPara);
int eTextPara::appendGlyph(Font *current_font, FT_Face current_face, FT_UInt glyphIndex, int flags, int rflags, int border, bool last,
		bool activate_newcolor, unsigned long newcolor)
{
	int xadvance, top, left, height;
	pGlyph ng;
	int xborder = 0;

	if (border)
	{
		/* TODO: scale border radius with current_font scaling */
		if (current_font->getGlyphImage(glyphIndex, &ng.image, &ng.borderimage, 64 * border))
			return 1;
		if (ng.image && ng.image->format != FT_GLYPH_FORMAT_BITMAP)
		{
			FT_Glyph_To_Bitmap(&ng.image, FT_RENDER_MODE_NORMAL, NULL, 1);
			if (ng.image->format != FT_GLYPH_FORMAT_BITMAP) return 1;
		}
		if (ng.borderimage && ng.borderimage->format != FT_GLYPH_FORMAT_BITMAP)
		{
			FT_Glyph_To_Bitmap(&ng.borderimage, FT_RENDER_MODE_NORMAL, NULL, 1);
			if (ng.borderimage->format != FT_GLYPH_FORMAT_BITMAP) return 1;
		}
		FT_BitmapGlyph glyph = NULL;
		if (ng.borderimage)
		{
			xadvance = ng.borderimage->advance.x;
			/*
			 * NOTE: our boundingbox calculation uses xadvance, and ignores glyph width.
			 * This is fine for all glyphs, except the last one (i.e. rightmost, for left-to-right rendering)
			 * For border glyphs, xadvance is significantly smaller than the glyph width.
			 * In fact, border glyphs often have the same xadvance as normal glyphs, borders
			 * are allowed to overlap.
			 * As a result, the boundingbox is calculated too small, the actual glyphs won't
			 * fit into it, and depending on the alignment, one of the borders on the sides
			 * will be cut off.
			 * Ideally, the boundingbox calculation should be rewritten, to use both advance and glyph dimensions.
			 * However, for now we adjust xadvance of the last glyph, so the current calculation will produce
			 * a better fitting boundingbox for border glyphs.
			 *
			 * The compensation equals half of the difference between 'normal' glyph width,
			 * and border glyph width. (half the width difference is on the left, and half on the right
			 * of the glyph, we only need to compensate for the part on the right)
			 * And since xadvance is in 16.16 units, we use (dW/2) << 16 = dW << 15
			 */
			if (last)
			{
				xadvance += (((FT_BitmapGlyph)ng.borderimage)->bitmap.width - ((FT_BitmapGlyph)ng.image)->bitmap.width) << 15;
			}
			if (!previous)
			{
				/* Move the first character, to make sure the border does not get cut off by the boundingbox (xborder is in pixel units, so just divide the width difference by two)  */
				xborder = (((FT_BitmapGlyph)ng.borderimage)->bitmap.width - ((FT_BitmapGlyph)ng.image)->bitmap.width) / 2;
			}
			glyph = (FT_BitmapGlyph)ng.borderimage;
		}
		else if (ng.image)
		{
			xadvance = ng.image->advance.x;
			glyph = (FT_BitmapGlyph)ng.image;
		}
		else
		{
			return 1;
		}
		xadvance >>= 16;

		top = glyph->top;
		left = glyph->left;
		height = glyph->bitmap.rows;
	}
	else
	{
		FTC_SBit glyph;
		if (current_font->getGlyphBitmap(glyphIndex, &glyph))
			return 1;

		xadvance = glyph->xadvance;
		top = glyph->top;
		left = glyph->left;
		height = glyph->height;
	}

	int nx=cursor.x();

	nx+=xadvance;

	if ((rflags & RS_WRAP) && (nx > area.right()))
	{
		int cnt = 0, maycnt = -1;
		glyphString::reverse_iterator i(glyphs.rbegin()), mayi(glyphs.rend());
			/* find first possibility (from end to begin) to break */
		while (i != glyphs.rend())
		{
			if (i->flags&(GS_CANBREAK|GS_ISFIRST)) /* stop on either space/hyphen/shy or start of line (giving up) */
				break;
			if ((i->flags&GS_MAYBREAK) && maycnt == -1)
			{
				maycnt = cnt;
				mayi = i;
			}
			cnt++;
			++i;
		}
		if (maycnt != -1 && (i == glyphs.rend() || !(i->flags&GS_CANBREAK)))
		{
			cnt = maycnt;
			i = mayi;
		}

			/* if ... */
		if (i != glyphs.rend()  /* ... we found anything */
			&& (i->flags&(GS_CANBREAK|GS_MAYBREAK)) /* ...and this is a space/hyphen/soft-hyphen */
			&& (!(i->flags & GS_ISFIRST)) /* ...and this is not an start of line (line with just a single space/hyphen) */
			&& cnt ) /* ... and there are actual non-space characters after this */
		{
				/* if we have a soft-hyphen, and used that for breaking, turn it into a real hyphen */
			if (i->flags & GS_SOFTHYPHEN)
			{
				i->flags &= ~GS_SOFTHYPHEN;
				i->flags |= GS_HYPHEN;
			}
			--i; /* skip the space/hyphen/softhyphen */
			int linelength=cursor.x()-i->x;
			i->flags|=GS_ISFIRST; /* make this a line start */
			ePoint offset=ePoint(i->x, i->y);
			newLine(rflags);
			offset-=cursor;

				/* and move everything to the end into the next line. */
			do
			{
				i->x-=offset.x();
				i->y-=offset.y();
				i->bbox.moveBy(-offset.x(), -offset.y());
				--lineChars.back();
				++charCount;
			} while (i-- != glyphs.rbegin()); // rearrange them into the next line
			cursor+=ePoint(linelength, 0);  // put the cursor after that line
		} else
		{
			if (cnt)
			{
				newLine(rflags);
				flags|=GS_ISFIRST;
			}
		}
	}

	int kern=0;
	if (previous && use_kerning)
	{
		FT_Vector delta;
		FT_Get_Kerning(current_face, previous, glyphIndex, ft_kerning_default, &delta);
		kern=delta.x>>6;
	}

	ng.bbox.setLeft(cursor.x() + left + xborder);
	ng.bbox.setTop( cursor.y() - top );
	ng.bbox.setHeight( height );

	xadvance += kern + xborder;
	ng.bbox.setWidth(xadvance);

	ng.x = cursor.x() + kern + xborder;
	ng.y = cursor.y();
	ng.w = xadvance;

	ng.font = current_font;
	ng.glyph_index = glyphIndex;
	ng.flags = flags;

	if (activate_newcolor)
	{
		ng.flags |= GS_COLORCHANGE;
		ng.newcolor = newcolor;
	}

	glyphs.push_back(ng);
	++charCount;

		/* when we have a SHY, don't xadvance. It will either be the last in the line (when used for breaking), or not displayed. */
	if (!(flags & GS_SOFTHYPHEN))
		cursor += ePoint(xadvance, 0);
	previous = glyphIndex;
	return 0;
}

void eTextPara::calc_bbox()
{
	if (!glyphs.size())
	{
		bboxValid = 0;
		boundBox = eRect();
		return;
	}

	bboxValid = 1;

	glyphString::iterator i(glyphs.begin());

	boundBox.setLeft(i->x);
	boundBox.setRight(i->bbox.right());

	while (++i != glyphs.end())
	{
		if (i->flags & (GS_ISSPACE|GS_SOFTHYPHEN))
			continue;
		if ( i->x < boundBox.left() )
			boundBox.setLeft( i->x );
		if ( i->bbox.right() > boundBox.right() )
			boundBox.setRight( i->bbox.right() );
	}
	boundBox.setTop(area.y());
	boundBox.setBottom(area.y() + totalheight);
//	eDebug("[eTextPara] boundBox left = %i, top = %i, right = %i, bottom = %i", boundBox.left(), boundBox.top(), boundBox.right(), boundBox.bottom() );
}

void eTextPara::newLine(int flags)
{
	if (maximum.width()<cursor.x())
		maximum.setWidth(cursor.x());
	cursor.setX(left);
	int height = current_face->size->metrics.height;
	if (!height)
	{
		/* some fonts don't have height filled in. Estimate it based on the bbox dimensions. */
		/* Usually, 'height' is less than the complete boundingbox height, so we use only yMax, to avoid getting a much too large line spacing */
		height = FT_MulFix(current_face->bbox.yMax, current_face->size->metrics.y_scale);
	}
	height >>= 6;

	lineOffsets.push_back(cursor.y());
	lineChars.push_back(charCount);
	charCount=0;

	cursor+=ePoint(0, height);
	if (maximum.height()<cursor.y())
		maximum.setHeight(cursor.y());
	previous=0;
	totalheight += height;
	lineCount++;
}

eTextPara::~eTextPara()
{
	clear();
}

void eTextPara::setFont(const gFont *font, int tabwidth)
{
	ePtr<Font> fnt, replacement, fallback;
	fontRenderClass::getInstance()->getFont(fnt, font->family.c_str(), font->pointSize, tabwidth);
	if (!fnt)
		eWarning("[eTextPara] Font '%s' is missing!", font->family.c_str());
	fontRenderClass::getInstance()->getFont(replacement, replacement_facename.c_str(), font->pointSize, tabwidth);
	fontRenderClass::getInstance()->getFont(fallback, fallback_facename.c_str(), font->pointSize, tabwidth);
	setFont(fnt, replacement, fallback);
}

std::string eTextPara::replacement_facename;
std::string eTextPara::fallback_facename;
std::set<int> eTextPara::forced_replaces;

void eTextPara::setFont(Font *fnt, Font *replacement, Font *fallback)
{
	if (!fnt)
		return;
	current_font=fnt;
	replacement_font=replacement;
	fallback_font=fallback;
	singleLock s(ftlock);

			// we ask for replacment_font first becauseof the cache
	if (replacement_font)
	{
		if ((FTC_Manager_LookupFace(fontRenderClass::instance->cacheManager,
					    replacement_font->scaler.face_id,
					    &replacement_face) < 0) ||
		    (FTC_Manager_LookupSize(fontRenderClass::instance->cacheManager,
					    &replacement_font->scaler,
					    &replacement_font->size) < 0))
		{
			eDebug("[eTextPara] setFont: FTC_Manager_Lookup_Size replacement_font failed!");
			return;
		}
	}
	if (current_font)
	{
		if ((FTC_Manager_LookupFace(fontRenderClass::instance->cacheManager,
					    current_font->scaler.face_id,
					    &current_face) < 0) ||
		    (FTC_Manager_LookupSize(fontRenderClass::instance->cacheManager,
					    &current_font->scaler,
					    &current_font->size) < 0))
		{
			eDebug("[eTextPara] setFont: FTC_Manager_Lookup_Size current_font failed!");
			return;
		}
	}
	if (fallback_font)
	{
		if ((FTC_Manager_LookupFace(fontRenderClass::instance->cacheManager,
					    fallback_font->scaler.face_id,
					    &fallback_face) < 0) ||
		    (FTC_Manager_LookupSize(fontRenderClass::instance->cacheManager,
					    &fallback_font->scaler,
					    &fallback_font->size) < 0))
		{
			eDebug("[eTextPara] FTC_Manager_Lookup_Size failed!");
			return;
		}
	}
	previous=0;
	use_kerning=FT_HAS_KERNING(current_face);
}

void
shape (std::vector<unsigned long> &string, const std::vector<unsigned long> &text);

int eTextPara::renderString(const char *string, int rflags, int border, int markedpos)
{
	singleLock s(ftlock);

	if (!current_font)
		return -1;

	if ((FTC_Manager_LookupFace(fontRenderClass::instance->cacheManager,
				current_font->scaler.face_id,
				&current_face) < 0) ||
	    (FTC_Manager_LookupSize(fontRenderClass::instance->cacheManager,
				&current_font->scaler,
				&current_font->size) < 0))
	{
		eDebug("[eTextPara] renderString: FTC_Manager_Lookup_Size current_font failed!");
		return -1;
	}

	if (cursor.y()==-1)
	{
		int height = current_face->size->metrics.height;
		int ascender = current_face->size->metrics.ascender;
		if (!height || !ascender)
		{
			int ymax = FT_MulFix(current_face->bbox.yMax, current_face->size->metrics.y_scale);
			if (!height)
			{
				/* some fonts don't have height filled in. Estimate it based on the bbox dimensions. */
				/* For the first line we calculate the full boundingbox height, this gives the best result when centering vertically */
				height = ymax - FT_MulFix(current_face->bbox.yMin, current_face->size->metrics.y_scale);
			}
			if (!ascender)
			{
				/* some fonts don't have ascender filled in. Estimate it based on the bbox dimensions. */
				ascender = ymax;
			}
		}
		totalheight = height >> 6;
		lineCount = 1;
		cursor=ePoint(area.x(), area.y()+(ascender>>6));
		left=cursor.x();
	}

	std::vector<unsigned long> uc_string, uc_visual;
	if (string)
		uc_string.reserve(strlen(string));

	const char *p = string ? string : "";

	while (*p)
	{
		unsigned int unicode=(unsigned char)*p++;

		if (unicode & 0x80) // we have (hopefully) UTF8 here, and we assume that the encoding is VALID
		{
			if ((unicode & 0xE0)==0xC0) // two bytes
			{
				unicode&=0x1F;
				unicode<<=6;
				if (*p)
					unicode|=(*p++)&0x3F;
			} else if ((unicode & 0xF0)==0xE0) // three bytes
			{
				unicode&=0x0F;
				unicode<<=6;
				if (*p)
					unicode|=(*p++)&0x3F;
				unicode<<=6;
				if (*p)
					unicode|=(*p++)&0x3F;
			} else if ((unicode & 0xF8)==0xF0) // four bytes
			{
				unicode&=0x07;
				unicode<<=6;
				if (*p)
					unicode|=(*p++)&0x3F;
				unicode<<=6;
				if (*p)
					unicode|=(*p++)&0x3F;
				unicode<<=6;
				if (*p)
					unicode|=(*p++)&0x3F;
			}
		}
		uc_string.push_back(unicode);
	}

	std::vector<unsigned long> uc_shape;

		// character -> glyph conversion
	shape(uc_shape, uc_string);

		// now do the usual logical->visual reordering
	int size=uc_shape.size();
	FriBidiCharType dir=FRIBIDI_TYPE_ON;
	uc_visual.resize(size);
		// gaaanz lahm, aber anders geht das leider nicht, sorry.
	FriBidiChar *array = new FriBidiChar[size];
	FriBidiChar *target = new FriBidiChar[size];
	std::copy(uc_shape.begin(), uc_shape.end(), array);
	if(!fribidi_log2vis(array, size, &dir, target, 0, 0, 0))
	{
		delete [] target;
		delete [] array;
		return -1;
	}
	uc_visual.assign(target, target+size);

	glyphs.reserve(size);

	unsigned long newcolor = 0;
	bool activate_newcolor = false;
	int nextflags = 0;
	int pos = 0;
	int markedlen = 0;
	if (markedpos > 0xFFFF)
	{
		markedlen = markedpos >> 16;
		markedpos &= 0xFFFF;
	}

	for (std::vector<unsigned long>::const_iterator i(uc_visual.begin());
		i != uc_visual.end(); ++i)
	{
		int isprintable=1;
		int flags = nextflags;
		unsigned long chr = *i;

		if (!(rflags&RS_DIRECT))
		{
			switch (chr)
			{
			case '\\':
			{
				if ((i + 1) != uc_visual.end())
				{
					unsigned long c = *(i+1);
					switch (c)
					{
						case 'n':
							++i;
							goto newline;
						case 't':
							++i;
							goto tab;
						case 'r':
							++i;
							goto nprint;
						case 'c':
						{
							char color[8];
							int codeidx;
							for (codeidx = 0; codeidx < 8; codeidx++)
							{
								if ((i + 2 + codeidx) == uc_visual.end()) break;
								color[codeidx] = (char)((*(i + 2 + codeidx)) & 0xff);
							}
							if (codeidx == 8)
							{
								newcolor = gRGB(color).argb();
								activate_newcolor = true;
								isprintable = 0;
								i += 1 + codeidx;
							}
							break;
						}
						default:
						;
					}
				}
				break;
			}
			case '\t':
tab:				isprintable=0;
				cursor+=ePoint(current_font->tabwidth, 0);
				cursor-=ePoint(cursor.x()%current_font->tabwidth, 0);
				if (!(nextflags&GS_ISFIRST))
					nextflags|=GS_FIXED;
				break;
			case 0x8A:
			case 0xE08A:
			case '\n':
newline:			isprintable=0;
				newLine(rflags);
				nextflags|=GS_ISFIRST;
				nextflags&=~GS_FIXED;
				break;
			case '\r':
			case 0x86: case 0xE086:
			case 0x87: case 0xE087:
nprint:				isprintable=0;
				break;
			case 0xAD: // soft-hyphen
				flags |= GS_SOFTHYPHEN;
				chr = 0x2010; /* hyphen */
				break;
			case 0x2010:
			case '-':
				flags |= GS_HYPHEN;
				break;
			case '/':
				flags |= GS_MAYBREAK;
				break;
			case ' ':
				flags|=GS_ISSPACE;
			default:
				break;
			}
		}
		if (isprintable)
		{
			if (markedpos == -2 || markedpos == pos++)
			{
				flags |= GS_INVERT;
				if (markedlen)
				{
					--markedlen;
					++markedpos;
				}
			}

			FT_UInt index = 0;

				/* FIXME: our font doesn't seem to have a hyphen, so use hyphen-minus for it. */
			if (chr == 0x2010)
				chr = '-';

			if (forced_replaces.find(chr) == forced_replaces.end())
				index=(rflags&RS_DIRECT)? chr : FT_Get_Char_Index(current_face, chr);

			if (!index)
			{
				if (replacement_face)
					index=(rflags&RS_DIRECT)? chr : FT_Get_Char_Index(replacement_face, chr);

				if (!index)
				{
					if (fallback_face)
						index=(rflags&RS_DIRECT)? chr : FT_Get_Char_Index(fallback_face, chr);
					if (!index)
						eDebug("[eTextPara] Unicode U+%4lx not present", chr);
					else
						appendGlyph(fallback_font, fallback_face, index, flags, rflags, border, i == uc_visual.end() - 1, activate_newcolor, newcolor);
				}
				else
					appendGlyph(replacement_font, replacement_face, index, flags, rflags, border, i == uc_visual.end() - 1, activate_newcolor, newcolor);
			} else
				appendGlyph(current_font, current_face, index, flags, rflags, border, i == uc_visual.end() - 1, activate_newcolor, newcolor);

			if (index)
			{
				nextflags = 0;
				activate_newcolor = false;
			}
		} else if (nextflags&GS_ISFIRST && !glyphs.empty())
		{
			// Newline found, mark the last glyph with the newline flag
			glyphs.back().flags |= GS_LF;
		}
	}
	bboxValid=false;
	calc_bbox();
	if (dir & FRIBIDI_MASK_RTL)
	{
		doTopBottomReordering=true;
	}
	if (charCount)
	{
		lineOffsets.push_back(cursor.y());
		lineChars.push_back(charCount);
		charCount=0;
	}
	delete [] target;
	delete [] array;
	return 0;
}

void eTextPara::blit(gDC &dc, const ePoint &offset, const gRGB &cbackground, const gRGB &foreground, bool border, bool marked)
{
	if (glyphs.empty()) return;

	singleLock s(ftlock);

	if (!current_font)
		return;

	if ((FTC_Manager_LookupFace(fontRenderClass::instance->cacheManager,
 				    current_font->scaler.face_id,
 				    &current_face) < 0) ||
	    (FTC_Manager_LookupSize(fontRenderClass::instance->cacheManager,
				    &current_font->scaler,
				    &current_font->size) < 0))
	{
		eDebug("[eTextPara] FTC_Manager_Lookup_Size failed!");
		return;
	}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wregister"

	ePtr<gPixmap> target;
	dc.getPixmap(target);
	gUnmanagedSurface *surface = target->surface;
	gRGB currentforeground = foreground;
	const gRGB background = (m_blend && surface->bpp == 32 && !marked) ? gRGB(currentforeground.r, currentforeground.g, currentforeground.b, 200) : cbackground;

	register int opcode = -1;

	__u32 lookup32_normal[16];
	__u32 lookup32_invert[16];
	__u16 *lookup16_normal = (__u16*)lookup32_normal; // shares the same memory
	__u16 *lookup16_invert = (__u16*)lookup32_invert;
	gColor *lookup8_normal = 0;
	gColor *lookup8_invert = (gColor*)lookup32_invert;
	__u32 *lookup32;
	__u16 *lookup16;
	gColor *lookup8;

	gRegion sarea(eRect(0, 0, surface->x, surface->y));
	gRegion clip = dc.getClip() & sarea;
	clip &= eRect(area.left() + offset.x(), area.top() + offset.y(), area.width(), area.height()+(current_face->size->metrics.ascender>>6));

	int buffer_stride=surface->stride;

	bool setcolor = true;
	std::list<int>::reverse_iterator line_offs_it(lineOffsets.rbegin());
	std::list<int>::iterator line_chars_it(lineChars.begin());
	int line_offs=0;
	int line_chars=0;
	for (glyphString::iterator i(glyphs.begin()); i != glyphs.end(); ++i, --line_chars)
	{
		while(!line_chars)
		{
			line_offs = *(line_offs_it++);
			line_chars = *(line_chars_it++);
		}
		if (i->flags & GS_COLORCHANGE)
		{
			/* don't do colorchanges in borders */
			if (!border)
			{
				currentforeground = i->newcolor;
				setcolor = true;
			}
		}
		if (setcolor)
		{
			setcolor = false;
			if (surface->bpp == 8)
			{
				if (surface->clut.data)
				{
					lookup8_normal=getColor(surface->clut, background, currentforeground).lookup;

					int i;
					for (i=0; i<16; ++i)
						lookup8_invert[i] = lookup8_normal[i^0xF];

					opcode=0;
				} else
					opcode=1;
			} 
			else if (surface->bpp == 32)
			{
				opcode = (m_blend) ? 4 : 3;

				for (int i=0; i<16; ++i)
				{
					unsigned char da = background.a, dr = background.r, dg = background.g, db = background.b;
#define BLEND(y, x, a) (y + (((x-y) * a)>>8))

					int sa = i * 16;
					if (sa < 256)
					{
						da = BLEND(background.a, currentforeground.a, sa) & 0xFF;
						dr = BLEND(background.r, currentforeground.r, sa) & 0xFF;
						dg = BLEND(background.g, currentforeground.g, sa) & 0xFF;
						db = BLEND(background.b, currentforeground.b, sa) & 0xFF;
					}
#undef BLEND
					da ^= 0xFF;
					lookup32_normal[i]=db | (dg << 8) | (dr << 16) | (da << 24);;
				}
				for (int i=0; i<16; ++i)
					lookup32_invert[i]=lookup32_normal[i^0xF];
			} 
			else if (surface->bpp == 16)
			{
				opcode=2;
				for (int i = 0; i != 16; ++i)
				{
#define BLEND(y, x, a) (y + (((x-y) * a)>>8))
					unsigned char da = background.a, dr = background.r, dg = background.g, db = background.b;
					int sa = i * 16;
					if (sa < 256)
					{
						dr = BLEND(background.r, foreground.r, sa) & 0xFF;
						dg = BLEND(background.g, foreground.g, sa) & 0xFF;
						db = BLEND(background.b, foreground.b, sa) & 0xFF;
					}
#undef BLEND
#if BYTE_ORDER == LITTLE_ENDIAN
					lookup16_normal[i] = bswap_16(((db >> 3) << 11) | ((dg >> 2) << 5) | (dr >> 3));
#else
					lookup16_normal[i] = ((db >> 3) << 11) | ((dg >> 2) << 5) | (dr >> 3);
#endif
					da ^= 0xFF;
				}
				for (int i=0; i<16; ++i)
					lookup16_invert[i]=lookup16_normal[i^0xF];
			} 
			else
			{
				eWarning("[eTextPara] Can't render to %dbpp!", surface->bpp);
				return;
			}
		}
		if (i->flags & GS_SOFTHYPHEN)
			continue;

		if (!(i->flags & GS_INVERT))
		{
			lookup8 = lookup8_normal;
			lookup16 = lookup16_normal;
			lookup32 = lookup32_normal;
		} 
		else
		{
			lookup8 = lookup8_invert;
			lookup16 = lookup16_invert;
			lookup32 = lookup32_invert;
		}

		int rxbase, rybase;
		__u8 *dbase;
		__u8 *sbase;
		int sxbase;
		int sybase;
		int pitch;
		if (i->image)
		{
			FT_BitmapGlyph glyph = border ? (FT_BitmapGlyph)i->borderimage : (FT_BitmapGlyph)i->image;
			if (!glyph->bitmap.buffer) continue;
			rxbase = i->x + glyph->left + offset.x();
			rybase = i->y - glyph->top + offset.y();
			rybase=(doTopBottomReordering ? line_offs : i->y) - glyph->top + offset.y();
			sbase = glyph->bitmap.buffer;
			sxbase = glyph->bitmap.width;
			sybase = glyph->bitmap.rows;
			pitch = glyph->bitmap.pitch;
		}
		else
		{
			static FTC_SBit glyph_bitmap;
			if (fontRenderClass::instance->getGlyphBitmap(&i->font->font, i->glyph_index, &glyph_bitmap))
				continue;
			rxbase=i->x+glyph_bitmap->left + offset.x();
			rybase=(doTopBottomReordering ? line_offs : i->y) - glyph_bitmap->top + offset.y();
			sbase=glyph_bitmap->buffer;
			sxbase=glyph_bitmap->width;
			sybase=glyph_bitmap->height;
			pitch = glyph_bitmap->pitch;
		}
		dbase = (__u8*)(surface->data)+buffer_stride*rybase+rxbase*surface->bypp;
		for (unsigned int c = 0; c < clip.rects.size(); ++c)
		{
			int rx = rxbase, ry = rybase;
			__u8 *d = dbase;
			__u8 *s = sbase;
			register int sx = sxbase;
			int sy = sybase;
			if ((sy+ry) >= clip.rects[c].bottom())
				sy = clip.rects[c].bottom()-ry;
			if ((sx+rx) >= clip.rects[c].right())
				sx = clip.rects[c].right()-rx;
			if (rx < clip.rects[c].left())
			{
				int diff=clip.rects[c].left()-rx;
				s+=diff;
				sx-=diff;
				rx+=diff;
				d+=diff*surface->bypp;
			}
			if (ry < clip.rects[c].top())
			{
				int diff=clip.rects[c].top()-ry;
				s+=diff*pitch;
				sy-=diff;
				ry+=diff;
				d+=diff*buffer_stride;
			}
			if ((sx>0) && (sy>0))
			{
				int extra_source_stride = pitch - sx;
				switch (opcode)
				{
				case 0: 		// 4bit lookup to 8bit
					{
						register int extra_buffer_stride = buffer_stride - sx;
						register __u8 *td=d;
						for (int ay = 0; ay < sy; ay++)
						{
							register int ax;

							for (ax=0; ax<sx; ax++)
							{
								register int b=(*s++)>>4;
								if(b)
									*td=lookup8[b];
								++td;
							}
							s += extra_source_stride;
							td += extra_buffer_stride;
						}
					}
					break;
				case 1:	// 8bit direct
					{
						register int extra_buffer_stride = buffer_stride - sx;
						register __u8 *td=d;
						for (int ay = 0; ay < sy; ay++)
						{
							register int ax;
							for (ax=0; ax<sx; ax++)
							{
								register int b=*s++;
								*td++^=b;
							}
							s += extra_source_stride;
							td += extra_buffer_stride;
						}
					}
					break;
				case 2: // 16bit
					{
						int extra_buffer_stride = (buffer_stride >> 1) - sx;
						register __u16 *td = (__u16*)d;
						for (int ay = 0; ay != sy; ay++)
						{
								register int ax;
								for (ax = 0; ax != sx; ax++)
								{
									register int b = (*s++) >> 4;
									if (b)
										*td = lookup16[b];
									++td;
								}
								s += extra_source_stride;
								td += extra_buffer_stride;
						}
					}
					break;
				case 3: // 32bit
					{
						register int extra_buffer_stride = (buffer_stride >> 2) - sx;
						register __u32 *td=(__u32*)d;
						for (int ay = 0; ay < sy; ay++)
						{
							register int ax;
							for (ax=0; ax<sx; ax++)
							{
								register int b=(*s++)>>4;
								if(b)
									*td=lookup32[b];
								++td;
							}
							s += extra_source_stride;
							td += extra_buffer_stride;
						}
					}
					break;
				case 4: // 32-bit blend
					{
						register int extra_buffer_stride = (buffer_stride >> 2) - sx;
						register __u32 *td = (__u32 *)d;
						for (int ay = 0; ay < sy; ay++)
						{
							register int ax;
							for (ax = 0; ax < sx; ax++)
							{
								register int b = (*s++) >> 4;
								if (b)
								{
									// unsigned char frame_a = (*td) >> 24 & 0xFF;
									unsigned char frame_r = (*td) >> 16 & 0xFF;
									unsigned char frame_g = (*td) >> 8 & 0xFF;
									unsigned char frame_b = (*td) & 0xFF;

									unsigned char da = lookup32[b] >> 24 & 0xFF;
									unsigned char dr = lookup32[b] >> 16 & 0xFF;
									unsigned char dg = lookup32[b] >> 8 & 0xFF;
									unsigned char db = lookup32[b] & 0xFF;

#define BLEND(y, x, a) (y + (((x-y) * a)>>8))
									frame_r = BLEND(frame_r, dr, da) & 0xFF;
									frame_g = BLEND(frame_g, dg, da) & 0xFF;
									frame_b = BLEND(frame_b, db, da) & 0xFF;
#undef BLEND
									*td = ((currentforeground.a ^ 0xFF) << 24) | (frame_r << 16) | (frame_g << 8) | frame_b;
								}
								++td;
							}
							s += extra_source_stride;
							td += extra_buffer_stride;
						}
					}
					break;
				}
			}
		}
	}
#pragma GCC diagnostic pop
}

void eTextPara::realign(int dir, int markedpos, int scrollpos)	// der code hier ist ein wenig merkwuerdig.
{
	glyphString::iterator begin(glyphs.begin()), c(glyphs.begin()), end(glyphs.begin()), last;
	if ((dir==dirLeft || (dir==dirBidi && !doTopBottomReordering)) && markedpos < 0)
		return;
	while (c != glyphs.end())
	{
		int linelength=0;
		int linespace=area.width();
		int numspaces=0, num=0;
		begin=end;

		ASSERT( end != glyphs.end());

		glyphString::iterator nonspace_end(begin);

			// zeilenende suchen
		do {
			last=end;
			++end;
			if (!(last->flags&GS_ISSPACE) && (end == glyphs.end() || end->flags&(GS_ISSPACE|GS_ISFIRST)))
				nonspace_end = end;
		} while ((end != glyphs.end()) && (!(end->flags&GS_ISFIRST)));
			// end zeigt jetzt auf begin der naechsten zeile

		for (c=begin; c!=nonspace_end; ++c)
		{
			if (dir == dirBlock && c->flags&GS_FIXED)
			{
				numspaces=0;
				num=0;
				linespace=area.width()-c->x;
				linelength=0;
				begin = c;
			}
			if (c->flags&GS_ISSPACE)
				numspaces++;
			linelength+=c->w;
			num++;
		}
		c = end;

		// Ensure the marked position is visible.
		bool offset_set = false;
		if (markedpos >= 0)
		{
			if (linelength > area.width())
			{
				if (markedpos >= (int)glyphs.size())
					markedpos = glyphs.size() - 1;
				eRect bbox = glyphs[markedpos].bbox;
				if (scrollpos == 50)
				{
					int mark_center = bbox.left() + bbox.width() / 2 - area.left();
					int area_center = area.width() / 2;
					// The mark is near the start, leave left aligned.
					if (mark_center < area_center)
						return;
					// The mark is near the end, set right aligned.
					if (mark_center > linelength - area_center)
						dir = dirRight;
					// Center on the mark.
					else
					{
						dir = dirCenter;
						m_offset = area_center - mark_center;
						offset_set = true;
					}
				}
				else
				{
					dir = dirRight;
					offset_set = true;
					int scroll_offset = area.width() * scrollpos / 100;
					if (bbox.left() + m_offset < area.left() + scroll_offset)
					{
						m_offset = area.left() + scroll_offset - bbox.left();
						// The mark is near the start, leave left aligned.
						if (m_offset >= 0)
						{
							m_offset = 0;
							return;
						}
					}
					else if (bbox.right() + m_offset >= area.right() - scroll_offset)
					{
						m_offset = area.right() - scroll_offset - bbox.right();
						// The mark is near the end, set right aligned.
						if (m_offset < area.width() - linelength)
							offset_set = false;
					}
					// else mark is visible, keep current offset
				}
			}
			else if (dir == dirLeft || (dir == dirBidi && !doTopBottomReordering))
				return;
		}

		switch (dir)
		{
		case dirCenterIfFits:
			// If the text is larger than the available space,
			// don't re-align but align left.
			if (linelength > area.width())
				return;
			dir = dirCenter;
			[[fallthrough]];
		case dirRight:
		case dirCenter:
		case dirBidi:
		{
			if (!offset_set)
			{
				m_offset=area.width()-linelength;
				if (dir==dirCenter)
					m_offset/=2;
			}
			while (begin != end)
			{
				begin->bbox.moveBy(m_offset,0);
				begin->x += m_offset;
				++begin;
			}
			break;
		}
		case dirBlock:
		{
			if (end == glyphs.end())
			{
				/* last line, use left alignment */
				continue;
			}

			if (!numspaces)
			{
				/* no spaces, use left alignment */
				continue;
			}

			if (last->flags & GS_LF)
			{
				/* line ends with a linefeed, use left alignment */
				continue;
			}

			int off=(linespace-linelength)*256/(numspaces?numspaces:(num-1));
			int curoff=0;

			while (begin != nonspace_end)
			{
				int doadd=0;
				if (begin->flags & GS_ISSPACE)
					doadd=1;
				begin->x+=curoff>>8;
				begin->bbox.moveBy(curoff>>8,0);
				if (doadd)
					curoff+=off;
				++begin;
			}
			curoff = (curoff+255) >> 8;
			while (begin != end) {
				begin->x+=curoff;
				begin->bbox.moveBy(curoff,0);
				++begin;
			}
			break;
		}
		}
	}
	bboxValid=false;
	calc_bbox();
}

void eTextPara::clear()
{
	singleLock s(ftlock);

	current_font = 0;
	replacement_font = 0;

	for (unsigned int i = 0; i < glyphs.size(); i++)
	{
		if (glyphs[i].image) FT_Done_Glyph(glyphs[i].image);
		if (glyphs[i].borderimage) FT_Done_Glyph(glyphs[i].borderimage);
	}
	glyphs.clear();
	totalheight = 0;
	lineCount = 0;
}

eAutoInitP0<fontRenderClass> init_fontRenderClass(eAutoInitNumbers::graphic-1, "Font Render Class");
