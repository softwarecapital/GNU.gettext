/* Output stream for CSS styled text, producing ANSI escape sequences.
   Copyright (C) 2006 Free Software Foundation, Inc.
   Written by Bruno Haible <bruno@clisp.org>, 2006.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.  */

#include <config.h>

/* Specification.  */
#include "term-styled-ostream.h"

#include <stdlib.h>

#include <cr-om-parser.h>
#include <cr-sel-eng.h>
#include <cr-style.h>
#include <cr-rgb.h>
#include <cr-fonts.h>

#include "term-ostream.h"
#include "hash.h"
#include "xalloc.h"


/* CSS matching works as follows:
   Suppose we have an element inside class "header" inside class "table".
   We pretend to have an XML tree that looks like this:

     (root)
       +----table
              +----header

   For each of these XML nodes, the CSS matching engine can report the
   matching CSS declarations.  We extract the CSS property values that
   matter for terminal styling and cache them.  */

/* Attributes that can be set on a character.  */
typedef struct
{
  term_color_t     color;
  term_color_t     bgcolor;
  term_weight_t    weight;
  term_posture_t   posture;
  term_underline_t underline;
} attributes_t;

struct term_styled_ostream : struct styled_ostream
{
fields:
  /* The destination stream.  */
  term_ostream_t destination;
  /* The CSS document.  */
  CRCascade *css_document;
  /* The CSS matching engine.  */
  CRSelEng *css_engine;
  /* The list of active XML elements, with a space before each.
     For example, in above example, it is " table header".  */
  char *curr_classes;
  size_t curr_classes_length;
  size_t curr_classes_allocated;
  /* A hash table mapping a list of classes (as a string) to an
     'attributes_t *'.  */
  hash_table cache;
  /* The current attributes.  */
  attributes_t *curr_attr;
};

/* Implementation of ostream_t methods.  */

static void
term_styled_ostream::write_mem (term_styled_ostream_t stream,
				const void *data, size_t len)
{
  term_ostream_set_color (stream->destination, stream->curr_attr->color);
  term_ostream_set_bgcolor (stream->destination, stream->curr_attr->bgcolor);
  term_ostream_set_weight (stream->destination, stream->curr_attr->weight);
  term_ostream_set_posture (stream->destination, stream->curr_attr->posture);
  term_ostream_set_underline (stream->destination, stream->curr_attr->underline);

  term_ostream_write_mem (stream->destination, data, len);
}

static void
term_styled_ostream::flush (term_styled_ostream_t stream)
{
  term_ostream_flush (stream->destination);
}

static void
term_styled_ostream::free (term_styled_ostream_t stream)
{
  term_ostream_free (stream->destination);
  cr_cascade_destroy (stream->css_document);
  cr_sel_eng_destroy (stream->css_engine);
  free (stream->curr_classes);
  {
    void *ptr = NULL;
    const void *key;
    size_t keylen;
    void *data;

    while (hash_iterate (&stream->cache, &ptr, &key, &keylen, &data) == 0)
      {
	free (data);
      }
  }
  hash_destroy (&stream->cache);
  free (stream);
}

/* Implementation of styled_ostream_t methods.  */

/* According to the CSS2 spec, sections 6.1 and 6.2, we need to do a
   propagation: specified values -> computed values -> actual values.
   The computed values are necessary.  libcroco does not compute them for us.
   The function cr_style_resolve_inherited_properties is also not sufficient:
   it handles only the case of inheritance, not the case of non-inheritance.
   So we write style accessors that fetch the computed value, doing the
   inheritance on the fly.
   We then compute the actual values from the computed values; for colors,
   this is done through the rgb_to_color method.  */

static term_color_t
style_compute_color_value (CRStyle *style, enum CRRgbProp which,
			   term_ostream_t stream)
{
  for (;;)
    {
      if (style == NULL)
	return COLOR_DEFAULT;
      if (cr_rgb_is_set_to_inherit (&style->rgb_props[which].sv))
	style = style->parent_style;
      else if (cr_rgb_is_set_to_transparent (&style->rgb_props[which].sv))
	/* A transparent color occurs as default background color, set by
	   cr_style_set_props_to_default_values.  */
	return COLOR_DEFAULT;
      else
	{
	  CRRgb rgb;
	  int r;
	  int g;
	  int b;

	  cr_rgb_copy (&rgb, &style->rgb_props[which].sv);
	  if (cr_rgb_compute_from_percentage (&rgb) != CR_OK)
	    abort ();
	  r = rgb.red & 0xff;
	  g = rgb.green & 0xff;
	  b = rgb.blue & 0xff;
	  return term_ostream_rgb_to_color (stream, r, g, b);
	}
    }
}

static term_weight_t
style_compute_font_weight_value (const CRStyle *style)
{
  int value = 0;
  for (;;)
    {
      if (style == NULL)
	value += 4;
      else
	switch (style->font_weight)
	  {
	  case FONT_WEIGHT_INHERIT:
	    style = style->parent_style;
	    continue;
	  case FONT_WEIGHT_BOLDER:
	    value += 1;
	    style = style->parent_style;
	    continue;
	  case FONT_WEIGHT_LIGHTER:
	    value -= 1;
	    style = style->parent_style;
	    continue;
	  case FONT_WEIGHT_100:
	    value += 1;
	    break;
	  case FONT_WEIGHT_200:
	    value += 2;
	    break;
	  case FONT_WEIGHT_300:
	    value += 3;
	    break;
	  case FONT_WEIGHT_400: case FONT_WEIGHT_NORMAL:
	    value += 4;
	    break;
	  case FONT_WEIGHT_500:
	    value += 5;
	    break;
	  case FONT_WEIGHT_600:
	    value += 6;
	    break;
	  case FONT_WEIGHT_700: case FONT_WEIGHT_BOLD:
	    value += 7;
	    break;
	  case FONT_WEIGHT_800:
	    value += 8;
	    break;
	  case FONT_WEIGHT_900:
	    value += 9;
	    break;
	  default:
	    abort ();
	  }
      /* Value >= 600 -> WEIGHT_BOLD.  Value <= 500 -> WEIGHT_NORMAL.  */
      return (value >= 6 ? WEIGHT_BOLD : WEIGHT_NORMAL);
    }
}

static term_posture_t
style_compute_font_posture_value (const CRStyle *style)
{
  for (;;)
    {
      if (style == NULL)
	return POSTURE_DEFAULT;
      switch (style->font_style)
	{
	case FONT_STYLE_INHERIT:
	  style = style->parent_style;
	  break;
	case FONT_STYLE_NORMAL:
	  return POSTURE_NORMAL;
	case FONT_STYLE_ITALIC:
	case FONT_STYLE_OBLIQUE:
	  return POSTURE_ITALIC;
	default:
	  abort ();
	}
    }
}

static term_underline_t
style_compute_text_underline_value (const CRStyle *style)
{
  /* Not supported by libcroco's CRStyle type!  */
  return UNDERLINE_DEFAULT;
}

/* Match the current list of CSS classes to the CSS and return the result.  */
static attributes_t *
match (term_styled_ostream_t stream)
{
  xmlNodePtr root;
  xmlNodePtr curr;
  char *p_end;
  char *p_start;
  CRStyle *curr_style;
  attributes_t *attr;

  /* Create a hierarchy of XML nodes.  */
  root = xmlNewNode (NULL, (const xmlChar *) "__root__");
  root->type = XML_ELEMENT_NODE;
  curr = root;
  p_end = &stream->curr_classes[stream->curr_classes_length];
  p_start = stream->curr_classes;
  while (p_start < p_end)
    {
      char *p;
      xmlNodePtr child;

      if (!(*p_start == ' '))
	abort ();
      p_start++;
      for (p = p_start; p < p_end && *p != ' '; p++)
	;

      /* Temporarily replace the ' ' by '\0'.  */
      *p = '\0';
      child = xmlNewNode (NULL, (const xmlChar *) p_start);
      child->type = XML_ELEMENT_NODE;
      xmlSetProp (child, (const xmlChar *) "class", (const xmlChar *) p_start);
      *p = ' ';

      if (xmlAddChild (curr, child) == NULL)
	/* Error! Shouldn't happen.  */
	abort ();

      curr = child;
      p_start = p;
    }

  /* Retrieve the matching CSS declarations.  */
  /* Not curr_style = cr_style_new (TRUE); because that assumes that the
     default foreground color is black and that the default background color
     is white, which is not necessarily true in a terminal context.  */
  curr_style = NULL;
  for (curr = root; curr != NULL; curr = curr->children)
    {
      CRStyle *parent_style = curr_style;
      curr_style = NULL;

      if (cr_sel_eng_get_matched_style (stream->css_engine,
					stream->css_document,
					curr,
					parent_style, &curr_style,
					FALSE) != CR_OK)
	abort ();
      if (curr_style == NULL)
	/* No declarations matched this node.  Inherit all values.  */
	curr_style = parent_style;
      else
	/* curr_style is a new style, inheriting from parent_style.  */
	;
    }

  /* Extract the CSS declarations that we can use.  */
  attr = XMALLOC (attributes_t);
  attr->color =
    style_compute_color_value (curr_style, RGB_PROP_COLOR,
			       stream->destination);
  attr->bgcolor =
    style_compute_color_value (curr_style, RGB_PROP_BACKGROUND_COLOR,
			       stream->destination);
  attr->weight = style_compute_font_weight_value (curr_style);
  attr->posture = style_compute_font_posture_value (curr_style);
  attr->underline = style_compute_text_underline_value (curr_style);

  /* Free the style chain.  */
  while (curr_style != NULL)
    {
      CRStyle *parent_style = curr_style->parent_style;

      cr_style_destroy (curr_style);
      curr_style = parent_style;
    }

  /* Free the XML nodes.  */
  xmlFreeNodeList (root);

  return attr;
}

/* Match the current list of CSS classes to the CSS and store the result in
   stream->curr_attr and in the cache.  */
static void
match_and_cache (term_styled_ostream_t stream)
{
  attributes_t *attr = match (stream);
  if (hash_insert_entry (&stream->cache,
			 stream->curr_classes, stream->curr_classes_length,
			 attr) == NULL)
    abort ();
  stream->curr_attr = attr;
}

static void
term_styled_ostream::begin_use_class (term_styled_ostream_t stream,
				      const char *classname)
{
  size_t classname_len;
  char *p;
  void *found;

  if (classname[0] == '\0' || strchr (classname, ' ') != NULL)
    /* Invalid classname argument.  */
    abort ();

  /* Push the classname onto the classname list.  */
  classname_len = strlen (classname);
  if (stream->curr_classes_length + 1 + classname_len + 1
      > stream->curr_classes_allocated)
    {
      size_t new_allocated = stream->curr_classes_length + 1 + classname_len + 1;
      if (new_allocated < 2 * stream->curr_classes_allocated)
	new_allocated = 2 * stream->curr_classes_allocated;

      stream->curr_classes = xrealloc (stream->curr_classes, new_allocated);
      stream->curr_classes_allocated = new_allocated;
    }
  p = &stream->curr_classes[stream->curr_classes_length];
  *p++ = ' ';
  memcpy (p, classname, classname_len);
  stream->curr_classes_length += 1 + classname_len;

  /* Uodate stream->curr_attr.  */
  if (hash_find_entry (&stream->cache,
		       stream->curr_classes, stream->curr_classes_length,
		       &found) < 0)
    match_and_cache (stream);
  else
    stream->curr_attr = (attributes_t *) found;
}

static void
term_styled_ostream::end_use_class (term_styled_ostream_t stream,
				    const char *classname)
{
  char *p_end;
  char *p_start;
  char *p;
  void *found;

  if (stream->curr_classes_length == 0)
    /* No matching call to begin_use_class.  */
    abort ();

  /* Remove the trailing classname.  */
  p_end = &stream->curr_classes[stream->curr_classes_length];
  p = p_end;
  while (*--p != ' ')
    ;
  p_start = p + 1;
  if (!(p_end - p_start == strlen (classname)
	&& memcmp (p_start, classname, p_end - p_start) == 0))
    /* The match ing call to begin_use_class used a different classname.  */
    abort ();
  stream->curr_classes_length = p - stream->curr_classes;

  /* Update stream->curr_attr.  */
  if (hash_find_entry (&stream->cache,
		       stream->curr_classes, stream->curr_classes_length,
		       &found) < 0)
    abort ();
  stream->curr_attr = (attributes_t *) found;
}

/* Constructor.  */

term_styled_ostream_t
term_styled_ostream_create (int fd, const char *filename,
			    const char *css_filename)
{
  term_styled_ostream_t stream =
    XMALLOC (struct term_styled_ostream_representation);
  CRStyleSheet *css_file_contents;

  stream->base.base.vtable = &term_styled_ostream_vtable;
  stream->destination = term_ostream_create (fd, filename);

  if (cr_om_parser_simply_parse_file ((const guchar *) css_filename,
				      CR_UTF_8, /* CR_AUTO is not supported */
				      &css_file_contents) != CR_OK)
    {
      term_ostream_free (stream->destination);
      free (stream);
      return NULL;
    }
  stream->css_document = cr_cascade_new (NULL, css_file_contents, NULL);
  stream->css_engine = cr_sel_eng_new ();

  stream->curr_classes_allocated = 60;
  stream->curr_classes = XNMALLOC (stream->curr_classes_allocated, char);
  stream->curr_classes_length = 0;

  hash_init (&stream->cache, 10);

  match_and_cache (stream);

  return stream;
}