/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2012-2016 Matthias Klumpp <matthias@tenstral.net>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the license, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * SECTION:as-xmldata
 * @short_description: AppStream data XML serialization.
 * @include: appstream.h
 *
 * Private class to serialize AppStream data into its XML representation and
 * deserialize the data again.
 * This class is used by #AsMetadata to read AppStream XML.
 *
 * See also: #AsComponent, #AsMetadata
 */

#include "as-xmldata.h"

#include <glib.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <string.h>

#include "as-utils.h"
#include "as-utils-private.h"
#include "as-metadata.h"
#include "as-component-private.h"

typedef struct
{
	gchar *locale;
	gchar *locale_short;
	gchar *origin;
	gchar *media_baseurl;
	gint default_priority;

	AsParserMode mode;
} AsXMLDataPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (AsXMLData, as_xmldata, G_TYPE_OBJECT)
#define GET_PRIVATE(o) (as_xmldata_get_instance_private (o))

static gchar	**as_xmldata_get_children_as_strv (AsXMLData *xdt, xmlNode *node, const gchar *element_name);

/**
 * as_xmldata_init:
 **/
static void
as_xmldata_init (AsXMLData *xdt)
{
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	priv->default_priority = 0;
	priv->mode = AS_PARSER_MODE_UPSTREAM;
}

/**
 * as_xmldata_finalize:
 **/
static void
as_xmldata_finalize (GObject *object)
{
	AsXMLData *xdt = AS_XMLDATA (object);
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	g_free (priv->locale);
	g_free (priv->locale_short);
	g_free (priv->origin);
	g_free (priv->media_baseurl);

	G_OBJECT_CLASS (as_xmldata_parent_class)->finalize (object);
}

/**
 * as_xmldata_initialize:
 * @xdt: An instance of #AsXMLData
 *
 * Initialize the XML handler.
 */
void
as_xmldata_initialize (AsXMLData *xdt, const gchar *locale, const gchar *origin, const gchar *media_baseurl, gint priority)
{
	g_auto(GStrv) strv = NULL;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	g_free (priv->locale);
	g_free (priv->locale_short);
	priv->locale = g_strdup (locale);

	strv = g_strsplit (priv->locale, "_", 0);
	priv->locale_short = g_strdup (strv[0]);

	g_free (priv->origin);
	priv->origin = g_strdup (origin);

	g_free (priv->media_baseurl);
	priv->media_baseurl = g_strdup (media_baseurl);

	priv->default_priority = priority;
}

/**
 * as_xmldata_get_node_value:
 */
static gchar*
as_xmldata_get_node_value (AsXMLData *xdt, xmlNode *node)
{
	gchar *content;
	content = (gchar*) xmlNodeGetContent (node);

	return content;
}

/**
 * as_xmldata_dump_node_children:
 */
static gchar*
as_xmldata_dump_node_children (AsXMLData *xdt, xmlNode *node)
{
	GString *str = NULL;
	xmlNode *iter;
	xmlBufferPtr nodeBuf;

	str = g_string_new ("");
	for (iter = node->children; iter != NULL; iter = iter->next) {
		/* discard spaces */
		if (iter->type != XML_ELEMENT_NODE) {
					continue;
		}

		nodeBuf = xmlBufferCreate();
		xmlNodeDump (nodeBuf, NULL, iter, 0, 1);
		if (str->len > 0)
			g_string_append (str, "\n");
		g_string_append_printf (str, "%s", (const gchar*) nodeBuf->content);
		xmlBufferFree (nodeBuf);
	}

	return g_string_free (str, FALSE);
}

/**
 * as_xmldata_get_node_locale:
 * @node: A XML node
 *
 * Returns: The locale of a node, if the node should be considered for inclusion.
 * %NULL if the node should be ignored due to a not-matching locale.
 */
gchar*
as_xmldata_get_node_locale (AsXMLData *xdt, xmlNode *node)
{
	gchar *lang;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	lang = (gchar*) xmlGetProp (node, (xmlChar*) "lang");

	if (lang == NULL) {
		lang = g_strdup ("C");
		goto out;
	}

	if (g_strcmp0 (priv->locale, "ALL") == 0) {
		/* we should read all languages */
		goto out;
	}

	if (g_strcmp0 (lang, priv->locale) == 0) {
		goto out;
	}

	if (g_strcmp0 (lang, priv->locale_short) == 0) {
		g_free (lang);
		lang = g_strdup (priv->locale);
		goto out;
	}

	/* If we are here, we haven't found a matching locale.
	 * In that case, we return %NULL to indicate that this element should not be added.
	 */
	g_free (lang);
	lang = NULL;

out:
	return lang;
}

static gchar**
as_xmldata_get_children_as_strv (AsXMLData *xdt, xmlNode* node, const gchar* element_name)
{
	GPtrArray *list;
	xmlNode *iter;
	gchar **res;
	g_return_val_if_fail (xdt != NULL, NULL);
	g_return_val_if_fail (element_name != NULL, NULL);
	list = g_ptr_array_new_with_free_func (g_free);

	for (iter = node->children; iter != NULL; iter = iter->next) {
		/* discard spaces */
		if (iter->type != XML_ELEMENT_NODE) {
					continue;
		}
		if (g_strcmp0 ((gchar*) iter->name, element_name) == 0) {
			gchar* content = NULL;
			content = (gchar*) xmlNodeGetContent (iter);
			if (content != NULL) {
				gchar *s;
				s = g_strdup (content);
				g_strstrip (s);
				g_ptr_array_add (list, s);
			}
			g_free (content);
		}
	}

	res = as_ptr_array_to_strv (list);
	g_ptr_array_unref (list);
	return res;
}

/**
 * as_xmldata_process_screenshot:
 */
static void
as_xmldata_process_screenshot (AsXMLData *xdt, xmlNode* node, AsScreenshot* scr)
{
	xmlNode *iter;
	gchar *node_name;
	gchar *content = NULL;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	for (iter = node->children; iter != NULL; iter = iter->next) {
		/* discard spaces */
		if (iter->type != XML_ELEMENT_NODE)
			continue;

		node_name = (gchar*) iter->name;
		content = as_xmldata_get_node_value (xdt, iter);
		g_strstrip (content);

		if (g_strcmp0 (node_name, "image") == 0) {
			g_autoptr(AsImage) img = NULL;
			guint64 width;
			guint64 height;
			gchar *stype;
			gchar *str;
			if (content == NULL) {
				continue;
			}
			img = as_image_new ();

			str = (gchar*) xmlGetProp (iter, (xmlChar*) "width");
			if (str == NULL) {
				width = 0;
			} else {
				width = g_ascii_strtoll (str, NULL, 10);
				g_free (str);
			}
			str = (gchar*) xmlGetProp (iter, (xmlChar*) "height");
			if (str == NULL) {
				height = 0;
			} else {
				height = g_ascii_strtoll (str, NULL, 10);
				g_free (str);
			}

			/* discard invalid elements */
			if (priv->mode == AS_PARSER_MODE_DISTRO) {
				/* no sizes are okay for upstream XML, but not for distro XML */
				if ((width == 0) || (height == 0)) {
					g_free (content);
					continue;
				}
			}

			as_image_set_width (img, width);
			as_image_set_height (img, height);

			stype = (gchar*) xmlGetProp (iter, (xmlChar*) "type");
			if (g_strcmp0 (stype, "thumbnail") == 0) {
				as_image_set_kind (img, AS_IMAGE_KIND_THUMBNAIL);
			} else {
				as_image_set_kind (img, AS_IMAGE_KIND_SOURCE);
			}
			g_free (stype);

			if (priv->media_baseurl == NULL) {
				/* no baseurl, we can just set the value as URL */
				as_image_set_url (img, content);
			} else {
				/* handle the media baseurl */
				gchar *tmp;
				tmp = g_build_filename (priv->media_baseurl, content, NULL);
				as_image_set_url (img, tmp);
				g_free (tmp);
			}

			as_screenshot_add_image (scr, img);
		} else if (g_strcmp0 (node_name, "caption") == 0) {
			if (content != NULL) {
				gchar *lang;
				lang = as_xmldata_get_node_locale (xdt, iter);
				if (lang != NULL)
					as_screenshot_set_caption (scr, content, lang);
				g_free (lang);
			}
		}
		g_free (content);
	}
}

/**
 * as_xmldata_process_screenshots_tag:
 */
static void
as_xmldata_process_screenshots_tag (AsXMLData *xdt, xmlNode* node, AsComponent* cpt)
{
	xmlNode *iter;
	AsScreenshot *sshot = NULL;
	gchar *prop;
	g_return_if_fail (xdt != NULL);
	g_return_if_fail (cpt != NULL);

	for (iter = node->children; iter != NULL; iter = iter->next) {
		/* discard spaces */
		if (iter->type != XML_ELEMENT_NODE)
			continue;

		if (g_strcmp0 ((gchar*) iter->name, "screenshot") == 0) {
			sshot = as_screenshot_new ();

			/* propagate locale */
			as_screenshot_set_active_locale (sshot, as_component_get_active_locale (cpt));

			prop = (gchar*) xmlGetProp (iter, (xmlChar*) "type");
			if (g_strcmp0 (prop, "default") == 0)
				as_screenshot_set_kind (sshot, AS_SCREENSHOT_KIND_DEFAULT);
			as_xmldata_process_screenshot (xdt, iter, sshot);
			if (as_screenshot_is_valid (sshot))
				as_component_add_screenshot (cpt, sshot);
			g_free (prop);
			g_object_unref (sshot);
		}
	}
}

/**
 * as_xmldata_upstream_description_to_cpt:
 *
 * Helper function for GHashTable
 */
static void
as_xmldata_upstream_description_to_cpt (gchar *key, GString *value, AsComponent *cpt)
{
	g_assert (AS_IS_COMPONENT (cpt));

	as_component_set_description (cpt, value->str, key);
	g_string_free (value, TRUE);
}

/**
 * as_xmldata_upstream_description_to_release:
 *
 * Helper function for GHashTable
 */
static void
as_xmldata_upstream_description_to_release (gchar *key, GString *value, AsRelease *rel)
{
	g_assert (AS_IS_RELEASE (rel));

	as_release_set_description (rel, value->str, key);
	g_string_free (value, TRUE);
}

/**
 * as_xmldata_parse_upstream_description_tag:
 */
static void
as_xmldata_parse_upstream_description_tag (AsXMLData *xdt, xmlNode* node, GHFunc func, gpointer entity)
{
	xmlNode *iter;
	gchar *node_name;
	GHashTable *desc;

	desc = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	for (iter = node->children; iter != NULL; iter = iter->next) {
		gchar *lang;
		gchar *content;
		GString *str;

		/* discard spaces */
		if (iter->type != XML_ELEMENT_NODE)
			continue;

		lang = as_xmldata_get_node_locale (xdt, iter);
		if (lang == NULL)
			/* this locale is not for us */
			continue;

		node_name = (gchar*) iter->name;
		content = as_xmldata_get_node_value (xdt, iter);

		str = g_hash_table_lookup (desc, lang);
		if (str == NULL) {
			str = g_string_new ("");
			g_hash_table_insert (desc, g_strdup (lang), str);
		}

		g_string_append_printf (str, "\n<%s>%s</%s>", node_name, content, node_name);
		g_free (lang);
		g_free (content);
	}

	g_hash_table_foreach (desc, func, entity);
	g_hash_table_unref (desc);
}

static void
as_xmldata_process_releases_tag (AsXMLData *xdt, xmlNode* node, AsComponent* cpt)
{
	xmlNode *iter;
	xmlNode *iter2;
	AsRelease *release = NULL;
	gchar *prop;
	guint64 timestamp;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);
	g_return_if_fail (cpt != NULL);

	for (iter = node->children; iter != NULL; iter = iter->next) {
		/* discard spaces */
		if (iter->type != XML_ELEMENT_NODE)
			continue;

		if (g_strcmp0 ((gchar*) iter->name, "release") == 0) {
			release = as_release_new ();

			/* propagate locale */
			as_release_set_active_locale (release, as_component_get_active_locale (cpt));

			prop = (gchar*) xmlGetProp (iter, (xmlChar*) "version");
			as_release_set_version (release, prop);
			g_free (prop);

			prop = (gchar*) xmlGetProp (iter, (xmlChar*) "date");
			if (prop != NULL) {
				g_autoptr(GDateTime) time;
				time = as_iso8601_to_datetime (prop);
				if (time != NULL) {
					as_release_set_timestamp (release, g_date_time_to_unix (time));
				} else {
					g_debug ("Invalid ISO-8601 date in releases of %s", as_component_get_id (cpt));
				}
				g_free (prop);
			}

			prop = (gchar*) xmlGetProp (iter, (xmlChar*) "timestamp");
			if (prop != NULL) {
				timestamp = g_ascii_strtoll (prop, NULL, 10);
				as_release_set_timestamp (release, timestamp);
				g_free (prop);
			}
			prop = (gchar*) xmlGetProp (iter, (xmlChar*) "urgency");
			if (prop != NULL) {
				AsUrgencyKind ukind;
				ukind = as_urgency_kind_from_string (prop);
				as_release_set_urgency (release, ukind);
				g_free (prop);
			}

			for (iter2 = iter->children; iter2 != NULL; iter2 = iter2->next) {
				gchar *content;

				if (iter->type != XML_ELEMENT_NODE)
					continue;

				if (g_strcmp0 ((gchar*) iter2->name, "location") == 0) {
					content = as_xmldata_get_node_value (xdt, iter2);
					as_release_add_location (release, content);
					g_free (content);
				} else if (g_strcmp0 ((gchar*) iter2->name, "checksum") == 0) {
					AsChecksumKind cs_kind;
					prop = (gchar*) xmlGetProp (iter2, (xmlChar*) "type");

					cs_kind = as_checksum_kind_from_string (prop);
					if (cs_kind != AS_CHECKSUM_KIND_NONE) {
						content = as_xmldata_get_node_value (xdt, iter2);
						as_release_set_checksum (release, content, cs_kind);
						g_free (content);
					}
					g_free (prop);
				} else if (g_strcmp0 ((gchar*) iter2->name, "size") == 0) {
					AsSizeKind s_kind;
					prop = (gchar*) xmlGetProp (iter2, (xmlChar*) "type");

					s_kind = as_size_kind_from_string (prop);
					if (s_kind != AS_SIZE_KIND_UNKNOWN) {
						guint64 size;

						content = as_xmldata_get_node_value (xdt, iter2);
						size = g_ascii_strtoull (content, NULL, 10);
						g_free (content);
						if (size > 0)
							as_release_set_size (release, size, s_kind);
					}
					g_free (prop);
				} else if (g_strcmp0 ((gchar*) iter2->name, "description") == 0) {
					if (priv->mode == AS_PARSER_MODE_DISTRO) {
						g_autofree gchar *lang;

						/* for distro XML, the "description" tag has a language property, so parsing it is simple */
						content = as_xmldata_dump_node_children (xdt, iter2);
						lang = as_xmldata_get_node_locale (xdt, iter2);
						if (lang != NULL)
							as_release_set_description (release, content, lang);
						g_free (content);
					} else {
						as_xmldata_parse_upstream_description_tag (xdt,
												iter2,
												(GHFunc) as_xmldata_upstream_description_to_release,
												release);
					}
				}
			}

			as_component_add_release (cpt, release);
			g_object_unref (release);
		}
	}
}

static void
as_xmldata_process_provides (AsXMLData *xdt, xmlNode* node, AsComponent* cpt)
{
	xmlNode *iter;
	gchar *node_name;
	gchar *content = NULL;
	g_return_if_fail (xdt != NULL);
	g_return_if_fail (cpt != NULL);

	for (iter = node->children; iter != NULL; iter = iter->next) {
		/* discard spaces */
		if (iter->type != XML_ELEMENT_NODE)
			continue;

		node_name = (gchar*) iter->name;
		content = as_xmldata_get_node_value (xdt, iter);
		if (content == NULL)
			continue;

		if (g_strcmp0 (node_name, "library") == 0) {
			as_component_add_provided_item (cpt, AS_PROVIDED_KIND_LIBRARY, content);
		} else if (g_strcmp0 (node_name, "binary") == 0) {
			as_component_add_provided_item (cpt, AS_PROVIDED_KIND_BINARY, content);
		} else if (g_strcmp0 (node_name, "font") == 0) {
			as_component_add_provided_item (cpt, AS_PROVIDED_KIND_FONT, content);
		} else if (g_strcmp0 (node_name, "modalias") == 0) {
			as_component_add_provided_item (cpt, AS_PROVIDED_KIND_MODALIAS, content);
		} else if (g_strcmp0 (node_name, "firmware") == 0) {
			g_autofree gchar *fwtype = NULL;
			fwtype = (gchar*) xmlGetProp (iter, (xmlChar*) "type");
			if (fwtype != NULL) {
				if (g_strcmp0 (fwtype, "runtime") == 0)
					as_component_add_provided_item (cpt, AS_PROVIDED_KIND_FIRMWARE_RUNTIME, content);
				else if (g_strcmp0 (fwtype, "flashed") == 0)
					as_component_add_provided_item (cpt, AS_PROVIDED_KIND_FIRMWARE_FLASHED, content);
			}
		} else if (g_strcmp0 (node_name, "python2") == 0) {
			as_component_add_provided_item (cpt, AS_PROVIDED_KIND_PYTHON_2, content);
		} else if (g_strcmp0 (node_name, "python3") == 0) {
			as_component_add_provided_item (cpt, AS_PROVIDED_KIND_PYTHON, content);
		} else if (g_strcmp0 (node_name, "dbus") == 0) {
			g_autofree gchar *dbustype = NULL;
			dbustype = (gchar*) xmlGetProp (iter, (xmlChar*) "type");

			if (g_strcmp0 (dbustype, "system") == 0)
				as_component_add_provided_item (cpt, AS_PROVIDED_KIND_DBUS_SYSTEM, content);
			else if ((g_strcmp0 (dbustype, "user") == 0) || (g_strcmp0 (dbustype, "session") == 0))
				as_component_add_provided_item (cpt, AS_PROVIDED_KIND_DBUS_USER, content);
		}

		g_free (content);
	}
}

static void
as_xmldata_set_component_type_from_node (xmlNode *node, AsComponent *cpt)
{
	gchar *cpttype;

	/* find out which kind of component we are dealing with */
	cpttype = (gchar*) xmlGetProp (node, (xmlChar*) "type");
	if ((cpttype == NULL) || (g_strcmp0 (cpttype, "generic") == 0)) {
		as_component_set_kind (cpt, AS_COMPONENT_KIND_GENERIC);
	} else {
		AsComponentKind ckind;
		ckind = as_component_kind_from_string (cpttype);
		as_component_set_kind (cpt, ckind);
		if (ckind == AS_COMPONENT_KIND_UNKNOWN)
			g_debug ("An unknown component was found: %s", cpttype);
	}
	g_free (cpttype);
}

static void
as_xmldata_process_languages_tag (AsXMLData *xdt, xmlNode* node, AsComponent* cpt)
{
	xmlNode *iter;
	gchar *prop;
	g_return_if_fail (xdt != NULL);
	g_return_if_fail (cpt != NULL);

	for (iter = node->children; iter != NULL; iter = iter->next) {
		/* discard spaces */
		if (iter->type != XML_ELEMENT_NODE)
			continue;

		if (g_strcmp0 ((gchar*) iter->name, "lang") == 0) {
			guint64 percentage = 0;
			gchar *locale;
			prop = (gchar*) xmlGetProp (iter, (xmlChar*) "percentage");
			if (prop != NULL) {
				percentage = g_ascii_strtoll (prop, NULL, 10);
				g_free (prop);
			}

			locale = as_xmldata_get_node_locale (xdt, iter);
			as_component_add_language (cpt, locale, percentage);
			g_free (locale);
		}
	}
}

/**
 * as_xmldata_parse_component_node:
 */
AsComponent*
as_xmldata_parse_component_node (AsXMLData *xdt, xmlNode* node, gboolean allow_invalid, GError **error)
{
	AsComponent* cpt;
	xmlNode *iter;
	const gchar *node_name;
	GPtrArray *compulsory_for_desktops;
	GPtrArray *pkgnames;
	gchar **strv;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	g_return_val_if_fail (xdt != NULL, NULL);

	compulsory_for_desktops = g_ptr_array_new_with_free_func (g_free);
	pkgnames = g_ptr_array_new_with_free_func (g_free);

	/* a fresh app component */
	cpt = as_component_new ();

	/* set component kind */
	as_xmldata_set_component_type_from_node (node, cpt);

	/* set the priority for this component */
	as_component_set_priority (cpt, priv->default_priority);

	/* set active locale for this component */
	as_component_set_active_locale (cpt, priv->locale);

	for (iter = node->children; iter != NULL; iter = iter->next) {
		gchar *content;
		gchar *lang;

		/* discard spaces */
		if (iter->type != XML_ELEMENT_NODE)
			continue;

		node_name = (const gchar*) iter->name;
		content = as_xmldata_get_node_value (xdt, iter);
		g_strstrip (content);
		lang = as_xmldata_get_node_locale (xdt, iter);

		if (g_strcmp0 (node_name, "id") == 0) {
				as_component_set_id (cpt, content);
				if ((priv->mode == AS_PARSER_MODE_UPSTREAM) &&
					(as_component_get_kind (cpt) == AS_COMPONENT_KIND_GENERIC)) {
					/* parse legacy component type information */
					as_xmldata_set_component_type_from_node (iter, cpt);
				}
		} else if (g_strcmp0 (node_name, "pkgname") == 0) {
			if (content != NULL)
				g_ptr_array_add (pkgnames, g_strdup (content));
		} else if (g_strcmp0 (node_name, "source_pkgname") == 0) {
			as_component_set_source_pkgname (cpt, content);
		} else if (g_strcmp0 (node_name, "name") == 0) {
			if (lang != NULL)
				as_component_set_name (cpt, content, lang);
		} else if (g_strcmp0 (node_name, "summary") == 0) {
			if (lang != NULL)
				as_component_set_summary (cpt, content, lang);
		} else if (g_strcmp0 (node_name, "description") == 0) {
			if (priv->mode == AS_PARSER_MODE_DISTRO) {
				/* for distro XML, the "description" tag has a language property, so parsing it is simple */
				if (lang != NULL) {
					gchar *desc;
					desc = as_xmldata_dump_node_children (xdt, iter);
					as_component_set_description (cpt, desc, lang);
					g_free (desc);
				}
			} else {
				as_xmldata_parse_upstream_description_tag (xdt,
									iter,
									(GHFunc) as_xmldata_upstream_description_to_cpt,
									cpt);
			}
		} else if (g_strcmp0 (node_name, "icon") == 0) {
			gchar *prop;
			g_autoptr(AsIcon) icon = NULL;
			if (content == NULL)
				continue;

			prop = (gchar*) xmlGetProp (iter, (xmlChar*) "type");
			icon = as_icon_new ();

			if (g_strcmp0 (prop, "stock") == 0) {
				as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
				as_icon_set_name (icon, content);
				as_component_add_icon (cpt, icon);
			} else if (g_strcmp0 (prop, "cached") == 0) {
				as_icon_set_kind (icon, AS_ICON_KIND_CACHED);
				as_icon_set_filename (icon, content);
				as_component_add_icon (cpt, icon);
			} else if (g_strcmp0 (prop, "local") == 0) {
				as_icon_set_kind (icon, AS_ICON_KIND_LOCAL);
				as_icon_set_filename (icon, content);
				as_component_add_icon (cpt, icon);
			} else if (g_strcmp0 (prop, "remote") == 0) {
				as_icon_set_kind (icon, AS_ICON_KIND_REMOTE);
				if (priv->media_baseurl == NULL) {
					/* no baseurl, we can just set the value as URL */
					as_icon_set_url (icon, content);
				} else {
					/* handle the media baseurl */
					gchar *tmp;
					tmp = g_build_filename (priv->media_baseurl, content, NULL);
					as_icon_set_url (icon, tmp);
					g_free (tmp);
				}
				as_component_add_icon (cpt, icon);
			}
		} else if (g_strcmp0 (node_name, "url") == 0) {
			if (content != NULL) {
				gchar *urltype_str;
				AsUrlKind url_kind;
				urltype_str = (gchar*) xmlGetProp (iter, (xmlChar*) "type");
				url_kind = as_url_kind_from_string (urltype_str);
				if (url_kind != AS_URL_KIND_UNKNOWN)
					as_component_add_url (cpt, url_kind, content);
				g_free (urltype_str);
			}
		} else if (g_strcmp0 (node_name, "categories") == 0) {
			gchar **cat_array;
			cat_array = as_xmldata_get_children_as_strv (xdt, iter, "category");
			as_component_set_categories (cpt, cat_array);
			g_strfreev (cat_array);
		} else if (g_strcmp0 (node_name, "keywords") == 0) {
			gchar **kw_array;
			kw_array = as_xmldata_get_children_as_strv (xdt, iter, "keyword");
			as_component_set_keywords (cpt, kw_array, NULL);
			g_strfreev (kw_array);
		} else if (g_strcmp0 (node_name, "mimetypes") == 0) {
			g_auto(GStrv) mime_array = NULL;
			guint i;

			/* Mimetypes are essentially provided interfaces, that's why they belong into Asprovided.
			 * However, due to historic reasons, the spec has an own toplevel tag for them, so we need
			 * to parse them here. */
			mime_array = as_xmldata_get_children_as_strv (xdt, iter, "mimetype");
			for (i = 0; mime_array[i] != NULL; i++) {
				as_component_add_provided_item (cpt, AS_PROVIDED_KIND_MIMETYPE, mime_array[i]);
			}
		} else if (g_strcmp0 (node_name, "provides") == 0) {
			as_xmldata_process_provides (xdt, iter, cpt);
		} else if (g_strcmp0 (node_name, "screenshots") == 0) {
			as_xmldata_process_screenshots_tag (xdt, iter, cpt);
		} else if (g_strcmp0 (node_name, "project_license") == 0) {
			if (content != NULL)
				as_component_set_project_license (cpt, content);
		} else if (g_strcmp0 (node_name, "project_group") == 0) {
			if (content != NULL)
				as_component_set_project_group (cpt, content);
		} else if (g_strcmp0 (node_name, "developer_name") == 0) {
			if (lang != NULL)
				as_component_set_developer_name (cpt, content, lang);
		} else if (g_strcmp0 (node_name, "compulsory_for_desktop") == 0) {
			if (content != NULL)
				g_ptr_array_add (compulsory_for_desktops, g_strdup (content));
		} else if (g_strcmp0 (node_name, "releases") == 0) {
			as_xmldata_process_releases_tag (xdt, iter, cpt);
		} else if (g_strcmp0 (node_name, "extends") == 0) {
			if (content != NULL)
				as_component_add_extends (cpt, content);
		} else if (g_strcmp0 (node_name, "languages") == 0) {
			as_xmldata_process_languages_tag (xdt, iter, cpt);
		} else if (g_strcmp0 (node_name, "bundle") == 0) {
			if (content != NULL) {
				gchar *type_str;
				AsBundleKind bundle_kind;
				type_str = (gchar*) xmlGetProp (iter, (xmlChar*) "type");
				bundle_kind = as_bundle_kind_from_string (type_str);
				if (bundle_kind != AS_BUNDLE_KIND_UNKNOWN)
					bundle_kind = AS_BUNDLE_KIND_LIMBA;
				as_component_add_bundle_id (cpt, bundle_kind, content);
				g_free (type_str);
			}
		}

		g_free (lang);
		g_free (content);
	}

	/* set the origin of this component */
	as_component_set_origin (cpt, priv->origin);

	/* add package name information to component */
	strv = as_ptr_array_to_strv (pkgnames);
	as_component_set_pkgnames (cpt, strv);
	g_ptr_array_unref (pkgnames);
	g_strfreev (strv);

	/* add compulsoriy information to component as strv */
	strv = as_ptr_array_to_strv (compulsory_for_desktops);
	as_component_set_compulsory_for_desktops (cpt, strv);
	g_ptr_array_unref (compulsory_for_desktops);
	g_strfreev (strv);

	if ((allow_invalid) || (as_component_is_valid (cpt))) {
		return cpt;
	} else {
		g_autofree gchar *cpt_str = NULL;
		cpt_str = as_component_to_string (cpt);
		g_set_error (error,
				     AS_METADATA_ERROR,
				     AS_METADATA_ERROR_FAILED,
				     "Invalid component: %s", cpt_str);
		g_object_unref (cpt);
	}

	return NULL;
}

/**
 * as_xmldata_parse_components_node:
 */
static void
as_xmldata_parse_components_node (AsXMLData *xdt, GPtrArray *cpts, xmlNode* node, gboolean allow_invalid, GError **error)
{
	AsComponent *cpt;
	xmlNode* iter;
	GError *tmp_error = NULL;
	gchar *media_baseurl;
	gchar *priority_str;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	/* set origin of this metadata */
	g_free (priv->origin);
	priv->origin = (gchar*) xmlGetProp (node, (xmlChar*) "origin");

	/* set baseurl for the media files */
	media_baseurl = (gchar*) xmlGetProp (node, (xmlChar*) "media_baseurl");
	priv->media_baseurl = media_baseurl;
	g_free (media_baseurl);

	/* distro metadata allows setting a priority for components */
	priority_str = (gchar*) xmlGetProp (node, (xmlChar*) "priority");
	if (priority_str != NULL) {
		priv->default_priority = g_ascii_strtoll (priority_str, NULL, 10);
	}
	g_free (priority_str);

	for (iter = node->children; iter != NULL; iter = iter->next) {
		/* discard spaces */
		if (iter->type != XML_ELEMENT_NODE)
			continue;

		if (g_strcmp0 ((gchar*) iter->name, "component") == 0) {
			cpt = as_xmldata_parse_component_node (xdt, iter, allow_invalid, &tmp_error);
			if (tmp_error != NULL) {
				g_propagate_error (error, tmp_error);
				return;
			} else if (cpt != NULL) {
				g_ptr_array_add (cpts, cpt);
			}
		}
	}
}

/**
 * as_component_xml_add_node:
 *
 * Add node if value is not empty
 */
static xmlNode*
as_xmldata_xml_add_node (xmlNode *root, const gchar *name, const gchar *value)
{
	if (as_str_empty (value))
		return NULL;

	return xmlNewTextChild (root, NULL, (xmlChar*) name, (xmlChar*) value);
}

/**
 * as_xmldata_xml_add_description:
 *
 * Add the description markup to the XML tree
 */
static gboolean
as_xmldata_xml_add_description (AsXMLData *xdt, xmlNode *root, xmlNode **desc_node, const gchar *description_markup, const gchar *lang)
{
	gchar *xmldata;
	xmlDoc *doc;
	xmlNode *droot;
	xmlNode *dnode;
	xmlNode *iter;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);
	gboolean ret = TRUE;
	gboolean localized;

	if (as_str_empty (description_markup))
		return FALSE;

	xmldata = g_strdup_printf ("<root>%s</root>", description_markup);
	doc = xmlParseDoc ((xmlChar*) xmldata);
	if (doc == NULL) {
		ret = FALSE;
		goto out;
	}

	droot = xmlDocGetRootElement (doc);
	if (droot == NULL) {
		ret = FALSE;
		goto out;
	}

	if (priv->mode == AS_PARSER_MODE_UPSTREAM) {
		if (*desc_node == NULL)
			*desc_node = xmlNewChild (root, NULL, (xmlChar*) "description", NULL);
		dnode = *desc_node;
	} else {
		/* in distro parser mode, we might have multiple <description/> tags */
		dnode = xmlNewChild (root, NULL, (xmlChar*) "description", NULL);
	}

	localized = g_strcmp0 (lang, "C") != 0;
	if (priv->mode != AS_PARSER_MODE_UPSTREAM) {
		if (localized) {
			xmlNewProp (dnode,
					(xmlChar*) "xml:lang",
					(xmlChar*) lang);
		}
	}

	for (iter = droot->children; iter != NULL; iter = iter->next) {
		xmlNode *cn;

		if ((g_strcmp0 ((const gchar*) iter->name, "ul") == 0) || (g_strcmp0 ((const gchar*) iter->name, "ol") == 0)) {
			xmlNode *iter2;
			xmlNode *enumNode;

			enumNode = xmlNewChild (dnode, NULL, iter->name, NULL);
			for (iter2 = iter->children; iter2 != NULL; iter2 = iter2->next) {
				cn = xmlAddChild (enumNode, xmlCopyNode (iter2, TRUE));

				if ((priv->mode == AS_PARSER_MODE_UPSTREAM) && (localized)) {
					xmlNewProp (cn,
						(xmlChar*) "xml:lang",
						(xmlChar*) lang);
				}
			}

			continue;
		}

		cn = xmlAddChild (dnode, xmlCopyNode (iter, TRUE));

		if ((priv->mode == AS_PARSER_MODE_UPSTREAM) && (localized)) {
			xmlNewProp (cn,
					(xmlChar*) "xml:lang",
					(xmlChar*) lang);
		}
	}

out:
	if (doc != NULL)
		xmlFreeDoc (doc);
	g_free (xmldata);
	return ret;
}

/**
 * as_component_xml_add_node_list:
 *
 * Add node if value is not empty
 */
static void
as_xmldata_xml_add_node_list (xmlNode *root, const gchar *name, const gchar *child_name, gchar **strv)
{
	xmlNode *node;
	guint i;

	if (strv == NULL)
		return;

	if (name == NULL)
		node = root;
	else
		node = xmlNewTextChild (root, NULL, (xmlChar*) name, NULL);
	for (i = 0; strv[i] != NULL; i++) {
		xmlNewTextChild (node, NULL, (xmlChar*) child_name, (xmlChar*) strv[i]);
	}
}

typedef struct {
	AsXMLData *xdt;

	xmlNode *parent;
	xmlNode *nd;
	const gchar *node_name;
} AsLocaleWriteHelper;

/**
 * _as_xmldata_lang_hashtable_to_nodes:
 */
static void
_as_xmldata_lang_hashtable_to_nodes (gchar *key, gchar *value, AsLocaleWriteHelper *helper)
{
	xmlNode *cnode;
	if (as_str_empty (value))
		return;

	cnode = xmlNewTextChild (helper->parent, NULL, (xmlChar*) helper->node_name, (xmlChar*) value);
	if (g_strcmp0 (key, "C") != 0) {
		xmlNewProp (cnode,
					(xmlChar*) "xml:lang",
					(xmlChar*) key);
	}
}

/**
 * _as_xmldata_desc_lang_hashtable_to_nodes:
 */
static void
_as_xmldata_desc_lang_hashtable_to_nodes (gchar *key, gchar *value, AsLocaleWriteHelper *helper)
{
	if (as_str_empty (value))
		return;

	as_xmldata_xml_add_description (helper->xdt, helper->parent, &helper->nd, value, key);
}

/**
 * _as_xmldata_serialize_image:
 */
static void
_as_xmldata_serialize_image (AsImage *img, xmlNode *subnode)
{
	xmlNode* n_image = NULL;
	gchar *size;
	g_return_if_fail (img != NULL);
	g_return_if_fail (subnode != NULL);

	n_image = xmlNewTextChild (subnode, NULL, (xmlChar*) "image", (xmlChar*) as_image_get_url (img));
	if (as_image_get_kind (img) == AS_IMAGE_KIND_THUMBNAIL)
		xmlNewProp (n_image, (xmlChar*) "type", (xmlChar*) "thumbnail");
	else
		xmlNewProp (n_image, (xmlChar*) "type", (xmlChar*) "source");

	if ((as_image_get_width (img) > 0) &&
		(as_image_get_height (img) > 0)) {
		size = g_strdup_printf("%i", as_image_get_width (img));
		xmlNewProp (n_image, (xmlChar*) "width", (xmlChar*) size);
		g_free (size);

		size = g_strdup_printf("%i", as_image_get_height (img));
		xmlNewProp (n_image, (xmlChar*) "height", (xmlChar*) size);
		g_free (size);
	}

	xmlAddChild (subnode, n_image);
}

/**
 * as_xmldata_add_screenshot_subnodes:
 *
 * Add screenshot subnodes to a root node
 */
static void
as_xmldata_add_screenshot_subnodes (AsComponent *cpt, xmlNode *root)
{
	GPtrArray* sslist;
	AsScreenshot *sshot;
	guint i;

	sslist = as_component_get_screenshots (cpt);
	for (i = 0; i < sslist->len; i++) {
		xmlNode *subnode;
		const gchar *str;
		GPtrArray *images;
		sshot = (AsScreenshot*) g_ptr_array_index (sslist, i);

		subnode = xmlNewTextChild (root, NULL, (xmlChar*) "screenshot", (xmlChar*) "");
		if (as_screenshot_get_kind (sshot) == AS_SCREENSHOT_KIND_DEFAULT)
			xmlNewProp (subnode, (xmlChar*) "type", (xmlChar*) "default");

		str = as_screenshot_get_caption (sshot);
		if (g_strcmp0 (str, "") != 0) {
			xmlNode* n_caption;
			n_caption = xmlNewTextChild (subnode, NULL, (xmlChar*) "caption", (xmlChar*) str);
			xmlAddChild (subnode, n_caption);
		}

		images = as_screenshot_get_images (sshot);
		g_ptr_array_foreach (images, (GFunc) _as_xmldata_serialize_image, subnode);
	}
}

/**
 * as_xmldata_add_release_subnodes:
 *
 * Add release nodes to a root node
 */
static void
as_xmldata_add_release_subnodes (AsXMLData *xdt, AsComponent *cpt, xmlNode *root)
{
	GPtrArray* releases;
	AsRelease *release;
	guint i;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	releases = as_component_get_releases (cpt);
	for (i = 0; i < releases->len; i++) {
		xmlNode *subnode;
		const gchar *str;
		glong unixtime;
		GPtrArray *locations;
		guint j;
		release = (AsRelease*) g_ptr_array_index (releases, i);

		/* set release version */
		subnode = xmlNewTextChild (root, NULL, (xmlChar*) "release", (xmlChar*) "");
		xmlNewProp (subnode, (xmlChar*) "version",
					(xmlChar*) as_release_get_version (release));

		/* set release timestamp / date */
		unixtime = as_release_get_timestamp (release);
		if (unixtime > 0 ) {
			g_autofree gchar *time_str = NULL;

			if (priv->mode == AS_PARSER_MODE_DISTRO) {
				time_str = g_strdup_printf ("%ld", unixtime);
				xmlNewProp (subnode, (xmlChar*) "timestamp",
						(xmlChar*) time_str);
			} else {
				GTimeVal time;
				time.tv_sec = unixtime;
				time.tv_usec = 0;
				time_str = g_time_val_to_iso8601 (&time);
				xmlNewProp (subnode, (xmlChar*) "date",
						(xmlChar*) time_str);
			}
		}

		/* set release urgency, if we have one */
		if (as_release_get_urgency (release) != AS_URGENCY_KIND_UNKNOWN) {
			const gchar *urgency_str;
			urgency_str = as_urgency_kind_to_string (as_release_get_urgency (release));
			xmlNewProp (subnode, (xmlChar*) "urgency",
					(xmlChar*) urgency_str);
		}

		/* add location urls */
		locations = as_release_get_locations (release);
		for (j = 0; j < locations->len; j++) {
			gchar *lurl;
			lurl = (gchar*) g_ptr_array_index (locations, j);
			xmlNewTextChild (subnode, NULL, (xmlChar*) "location", (xmlChar*) lurl);
		}

		/* add checksum node */
		for (j = 0; j < AS_CHECKSUM_KIND_LAST; j++) {
			if (as_release_get_checksum (release, j) != NULL) {
				xmlNode *cs_node;
				cs_node = xmlNewTextChild (subnode,
								NULL,
								(xmlChar*) "checksum",
								(xmlChar*) as_release_get_checksum (release, j));
				xmlNewProp (cs_node,
						(xmlChar*) "type",
						(xmlChar*) as_checksum_kind_to_string (j));
			}
		}

		/* add size node */
		for (j = 0; j < AS_SIZE_KIND_LAST; j++) {
			if (as_release_get_size (release, j) > 0) {
				xmlNode *s_node;
				g_autofree gchar *size_str;

				size_str = g_strdup_printf ("%" G_GUINT64_FORMAT, as_release_get_size (release, j));
				s_node = xmlNewTextChild (subnode,
							  NULL,
							  (xmlChar*) "size",
							  (xmlChar*) size_str);
				xmlNewProp (s_node,
						(xmlChar*) "type",
						(xmlChar*) as_size_kind_to_string (j));
			}
		}

		/* add description */
		str = as_release_get_description (release);
		if (g_strcmp0 (str, "") != 0) {
			xmlNode* n_desc;
			n_desc = xmlNewTextChild (subnode, NULL, (xmlChar*) "description", (xmlChar*) str);
			xmlAddChild (subnode, n_desc);
		}
	}
}

/**
 * as_xmldata_component_to_node:
 * @cpt: a valid #AsComponent
 *
 * Serialize the component data to an xmlNode.
 *
 */
static xmlNode*
as_xmldata_component_to_node (AsXMLData *xdt, AsComponent *cpt)
{
	xmlNode *cnode;
	xmlNode *node;
	gchar **strv;
	GPtrArray *releases;
	GPtrArray *screenshots;
	GPtrArray *icons;
	AsComponentKind kind;
	AsLocaleWriteHelper helper;
	guint i;
	g_return_val_if_fail (cpt != NULL, NULL);

	/* define component root node */
	kind = as_component_get_kind (cpt);
	cnode = xmlNewNode (NULL, (xmlChar*) "component");
	if ((kind != AS_COMPONENT_KIND_GENERIC) && (kind != AS_COMPONENT_KIND_UNKNOWN)) {
		xmlNewProp (cnode, (xmlChar*) "type",
					(xmlChar*) as_component_kind_to_string (kind));
	}

	as_xmldata_xml_add_node (cnode, "id", as_component_get_id (cpt));

	helper.parent = cnode;
	helper.xdt = xdt;
	helper.nd = NULL;
	helper.node_name = "name";
	g_hash_table_foreach (as_component_get_name_table (cpt),
					(GHFunc) _as_xmldata_lang_hashtable_to_nodes,
					&helper);

	helper.nd = NULL;
	helper.node_name = "summary";
	g_hash_table_foreach (as_component_get_summary_table (cpt),
					(GHFunc) _as_xmldata_lang_hashtable_to_nodes,
					&helper);

	helper.nd = NULL;
	helper.node_name = "developer_name";
	g_hash_table_foreach (as_component_get_developer_name_table (cpt),
					(GHFunc) _as_xmldata_lang_hashtable_to_nodes,
					&helper);

	helper.nd = NULL;
	helper.node_name = "description";
	g_hash_table_foreach (as_component_get_description_table (cpt),
					(GHFunc) _as_xmldata_desc_lang_hashtable_to_nodes,
					&helper);

	as_xmldata_xml_add_node (cnode, "project_license", as_component_get_project_license (cpt));
	as_xmldata_xml_add_node (cnode, "project_group", as_component_get_project_group (cpt));

	as_xmldata_xml_add_node_list (cnode, NULL, "pkgname", as_component_get_pkgnames (cpt));
	strv = as_ptr_array_to_strv (as_component_get_extends (cpt));
	as_xmldata_xml_add_node_list (cnode, NULL, "extends", strv);
	g_strfreev (strv);
	as_xmldata_xml_add_node_list (cnode, NULL, "compulsory_for_desktop", as_component_get_compulsory_for_desktops (cpt));
	as_xmldata_xml_add_node_list (cnode, "keywords", "keyword", as_component_get_keywords (cpt));
	as_xmldata_xml_add_node_list (cnode, "categories", "category", as_component_get_categories (cpt));

	/* urls */
	for (i = AS_URL_KIND_UNKNOWN; i < AS_URL_KIND_LAST; i++) {
		xmlNode *n;
		const gchar *value;
		value = as_component_get_url (cpt, i);
		if (value == NULL)
			continue;

		n = xmlNewTextChild (cnode, NULL, (xmlChar*) "url", (xmlChar*) value);
		xmlNewProp (n, (xmlChar*) "type",
					(xmlChar*) as_url_kind_to_string (i));
	}

	/* icons */
	icons = as_component_get_icons (cpt);
	for (i = 0; i < icons->len; i++) {
		AsIcon *icon = AS_ICON (g_ptr_array_index (icons, i));
		xmlNode *n;
		const gchar *value;

		if (as_icon_get_kind (icon) == AS_ICON_KIND_LOCAL)
			value = as_icon_get_filename (icon);
		else if (as_icon_get_kind (icon) == AS_ICON_KIND_REMOTE)
			value = as_icon_get_url (icon);
		else
			value = as_icon_get_name (icon);

		if (value == NULL)
			continue;

		n = xmlNewTextChild (cnode, NULL, (xmlChar*) "icon", (xmlChar*) value);
		xmlNewProp (n, (xmlChar*) "type",
					(xmlChar*) as_icon_kind_to_string (as_icon_get_kind (icon)));

		/* TODO: Prevent adding the same icon node multiple times? */
	}

	/* bundles */
	for (i = AS_BUNDLE_KIND_UNKNOWN; i < AS_BUNDLE_KIND_LAST; i++) {
		xmlNode *n;
		const gchar *value;
		value = as_component_get_bundle_id (cpt, i);
		if (value == NULL)
			continue;

		n = xmlNewTextChild (cnode, NULL, (xmlChar*) "bundle", (xmlChar*) value);
		xmlNewProp (n, (xmlChar*) "type",
					(xmlChar*) as_bundle_kind_to_string (i));
	}

	/* releases node */
	releases = as_component_get_releases (cpt);
	if (releases->len > 0) {
		node = xmlNewTextChild (cnode, NULL, (xmlChar*) "releases", NULL);
		as_xmldata_add_release_subnodes (xdt, cpt, node);
	}

	/* screenshots node */
	screenshots = as_component_get_screenshots (cpt);
	if (screenshots->len > 0) {
		node = xmlNewTextChild (cnode, NULL, (xmlChar*) "screenshots", NULL);
		as_xmldata_add_screenshot_subnodes (cpt, node);
	}

	return cnode;
}

/**
 * as_xmldata_parse_upstream_data:
 * @xdt: An instance of #AsXMLData
 * @data: XML representing upstream metadata.
 * @error: A #GError
 *
 * Parse AppStream upstream metadata.
 *
 * Returns: (transfer full): An #AsComponent, deserialized from the XML.
 */
AsComponent*
as_xmldata_parse_upstream_data (AsXMLData *xdt, const gchar *data, GError **error)
{
	xmlDoc* doc;
	xmlNode* root;
	AsComponent *cpt = NULL;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	if (data == NULL) {
		/* empty document means no components */
		return NULL;
	}

	doc = xmlParseDoc ((xmlChar*) data);
	if (doc == NULL) {
		g_set_error_literal (error,
				     AS_METADATA_ERROR,
				     AS_METADATA_ERROR_FAILED,
				     "Could not parse XML!");
		return NULL;
	}

	root = xmlDocGetRootElement (doc);
	if (root == NULL) {
		g_set_error_literal (error,
				     AS_METADATA_ERROR,
				     AS_METADATA_ERROR_FAILED,
				     "The XML document appears to be empty.");
		goto out;
	}

	/* switch to upstream format parsing */
	priv->mode = AS_PARSER_MODE_UPSTREAM;

	if (g_strcmp0 ((gchar*) root->name, "components") == 0) {
		g_set_error_literal (error,
				     AS_METADATA_ERROR,
				     AS_METADATA_ERROR_UNEXPECTED_FORMAT_KIND,
				     "Tried to parse distro metadata as upstream metadata.");
		goto out;
	} else if (g_strcmp0 ((gchar*) root->name, "component") == 0) {
		cpt = as_xmldata_parse_component_node (xdt, root, TRUE, error);
	} else if  (g_strcmp0 ((gchar*) root->name, "application") == 0) {
		g_debug ("Parsing legacy AppStream metadata file.");
		cpt = as_xmldata_parse_component_node (xdt, root, TRUE, error);
	} else {
		g_set_error_literal (error,
					AS_METADATA_ERROR,
					AS_METADATA_ERROR_FAILED,
					"XML file does not contain valid AppStream data!");
		goto out;
	}

out:
	xmlFreeDoc (doc);
	return cpt;
}

/**
 * as_xmldata_parse_distro_data:
 * @xdt: An instance of #AsXMLData
 * @data: XML representing distro metadata.
 * @error: A #GError
 *
 * Parse AppStream upstream metadata.
 *
 * Returns: (transfer full) (element-type AsComponent): An array of #AsComponent, deserialized from the XML.
 */
GPtrArray*
as_xmldata_parse_distro_data (AsXMLData *xdt, const gchar *data, GError **error)
{
	xmlDoc* doc;
	xmlNode* root;
	GPtrArray *cpts = NULL;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	if (data == NULL) {
		/* empty document means no components */
		return NULL;
	}

	doc = xmlParseDoc ((xmlChar*) data);
	if (doc == NULL) {
		g_set_error_literal (error,
				     AS_METADATA_ERROR,
				     AS_METADATA_ERROR_FAILED,
				     "Could not parse XML!");
		return NULL;
	}

	root = xmlDocGetRootElement (doc);
	if (root == NULL) {
		g_set_error_literal (error,
				     AS_METADATA_ERROR,
				     AS_METADATA_ERROR_FAILED,
				     "The XML document is empty.");
		goto out;
	}

	priv->mode = AS_PARSER_MODE_DISTRO;
	cpts = g_ptr_array_new_with_free_func (g_object_unref);

	if (g_strcmp0 ((gchar*) root->name, "components") == 0) {
		as_xmldata_parse_components_node (xdt, cpts, root, FALSE, error);
	} else if (g_strcmp0 ((gchar*) root->name, "component") == 0) {
		AsComponent *cpt;
		/* we explicitly allow parsing single component entries in distro-XML mode, since this is a scenario
		 * which might very well happen, e.g. in AppStream metadata generators */
		cpt = as_xmldata_parse_component_node (xdt, root, TRUE, error);
		if (cpt != NULL)
			g_ptr_array_add (cpts, cpt);
	} else {
		g_set_error_literal (error,
					AS_METADATA_ERROR,
					AS_METADATA_ERROR_FAILED,
					"XML file does not contain valid AppStream data!");
		goto out;
	}

out:
	xmlFreeDoc (doc);
	return cpts;
}

/**
 * as_xmldata_serialize_to_upstream:
 * @xdt: An instance of #AsXMLData
 * @cpt: The component to serialize.
 *
 * Serialize an #AsComponent to upstream XML.
 *
 * Returns: XML metadata.
 */
gchar*
as_xmldata_serialize_to_upstream (AsXMLData *xdt, AsComponent *cpt)
{
	xmlDoc *doc;
	xmlNode *root;
	gchar *xmlstr = NULL;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	priv->mode = AS_PARSER_MODE_UPSTREAM;
	doc = xmlNewDoc ((xmlChar*) NULL);

	/* define component root node */
	root = as_xmldata_component_to_node (xdt, cpt);
	if (root == NULL)
		goto out;
	xmlDocSetRootElement (doc, root);

out:
	xmlDocDumpMemory (doc, (xmlChar**) (&xmlstr), NULL);
	xmlFreeDoc (doc);

	return xmlstr;
}

/**
 * as_xmldata_serialize_to_distro:
 * @xdt: An instance of #AsXMLData
 * @cpt: The component to serialize.
 *
 * Serialize an #AsComponent to distro XML.
 *
 * Returns: XML metadata.
 */
gchar*
as_xmldata_serialize_to_distro (AsXMLData *xdt, GPtrArray *cpts)
{
	xmlDoc *doc;
	xmlNode *root;
	gchar *xmlstr = NULL;
	guint i;
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);

	if (cpts->len == 0)
		return NULL;

	priv->mode = AS_PARSER_MODE_DISTRO;
	root = xmlNewNode (NULL, (xmlChar*) "components");
	xmlNewProp (root, (xmlChar*) "version", (xmlChar*) "0.8");
	if (priv->origin != NULL)
		xmlNewProp (root, (xmlChar*) "origin", (xmlChar*) priv->origin);

	for (i = 0; i < cpts->len; i++) {
		AsComponent *cpt;
		xmlNode *node;
		cpt = AS_COMPONENT (g_ptr_array_index (cpts, i));

		node = as_xmldata_component_to_node (xdt, cpt);
		if (node == NULL)
			continue;
		xmlAddChild (root, node);
	}

	doc = xmlNewDoc ((xmlChar*) NULL);
	xmlDocSetRootElement (doc, root);

	xmlDocDumpMemory (doc, (xmlChar**) (&xmlstr), NULL);
	xmlFreeDoc (doc);

	return xmlstr;
}

/**
 * as_xmldata_set_parser_mode:
 */
void
as_xmldata_set_parser_mode (AsXMLData *xdt, AsParserMode mode)
{
	AsXMLDataPrivate *priv = GET_PRIVATE (xdt);
	priv->mode = mode;
}

/**
 * as_xmldata_class_init:
 **/
static void
as_xmldata_class_init (AsXMLDataClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = as_xmldata_finalize;
}

/**
 * as_xmldata_new:
 */
AsXMLData*
as_xmldata_new (void)
{
	AsXMLData *xdt;
	xdt = g_object_new (AS_TYPE_XMLDATA, NULL);
	return AS_XMLDATA (xdt);
}
