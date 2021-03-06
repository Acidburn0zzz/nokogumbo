//
// nokogumbo.c defines the following:
//
//   class Nokogumbo
//     def parse(utf8_string) # returns Nokogiri::HTML5::Document
//   end
//
// Processing starts by calling gumbo_parse_with_options.  The resulting
// document tree is then walked:
//
//  * if Nokogiri and libxml2 headers are available at compile time,
//    (ifdef NGLIB) then a parallel libxml2 tree is constructed, and the
//    final document is then wrapped using Nokogiri_wrap_xml_document.
//    This approach reduces memory and CPU requirements as Ruby objects
//    are only built when necessary.
//
//  * if the necessary headers are not available at compile time, Nokogiri
//    methods are called instead, producing the equivalent functionality.
//

#include <assert.h>
#include <ruby.h>
#include "gumbo.h"
#include "error.h"

// class constants
static VALUE Document;

#ifdef NGLIB
#include <nokogiri.h>
#include <xml_syntax_error.h>
#include <libxml/tree.h>
#include <libxml/HTMLtree.h>

#define NIL NULL
#define CONST_CAST (xmlChar const*)
#else
#define NIL Qnil
#define CONST_CAST

// more class constants
static VALUE cNokogiriXmlSyntaxError;

static VALUE Element;
static VALUE Text;
static VALUE CDATA;
static VALUE Comment;

// interned symbols
static VALUE new;
static VALUE attribute;
static VALUE set_attribute;
static VALUE remove_attribute;
static VALUE add_child;
static VALUE internal_subset;
static VALUE remove_;
static VALUE create_internal_subset;
static VALUE key_;
static VALUE node_name_;

// map libxml2 types to Ruby VALUE
#define xmlNodePtr VALUE
#define xmlDocPtr VALUE

// redefine libxml2 API as Ruby function calls
#define xmlNewDocNode(doc, ns, name, content) \
  rb_funcall(Element, new, 2, rb_str_new2(name), doc)
#define xmlNewDocText(doc, text) \
  rb_funcall(Text, new, 2, rb_str_new2(text), doc)
#define xmlNewCDataBlock(doc, content, length) \
  rb_funcall(CDATA, new, 2, doc, rb_str_new(content, length))
#define xmlNewDocComment(doc, text) \
  rb_funcall(Comment, new, 2, doc, rb_str_new2(text))
#define xmlAddChild(element, node) \
  rb_funcall(element, add_child, 1, node)
#define xmlDocSetRootElement(doc, root) \
  rb_funcall(doc, add_child, 1, root)
#define xmlCreateIntSubset(doc, name, external, system) \
  rb_funcall(doc, create_internal_subset, 3, rb_str_new2(name), \
    (external ? rb_str_new2(external) : Qnil), \
    (system ? rb_str_new2(system) : Qnil));
#define Nokogiri_wrap_xml_document(klass, doc) \
  doc

static VALUE find_dummy_key(VALUE collection) {
  VALUE r_dummy = Qnil;
  char dummy[5] = "a";
  size_t len = 1;
  while (len < sizeof dummy) {
    r_dummy = rb_str_new(dummy, len);
    if (rb_funcall(collection, key_, 1, r_dummy) == Qfalse)
      return r_dummy;
    for (size_t i = 0; ; ++i) {
      if (dummy[i] == 0) {
        dummy[i] = 'a';
        ++len;
        break;
      }
      if (dummy[i] == 'z')
        dummy[i] = 'a';
      else {
        ++dummy[i];
        break;
      }
    }
  }
  // This collection has 475254 elements?? Give up.
  return Qnil;
}

static xmlNodePtr xmlNewProp(xmlNodePtr node, const char *name, const char *value) {
  // Nokogiri::XML::Node#set_attribute calls xmlSetProp(node, name, value)
  // which behaves roughly as
  // if name is a QName prefix:local
  //   if node->doc has a namespace ns corresponding to prefix
  //     return xmlSetNsProp(node, ns, local, value)
  // return xmlSetNsProp(node, NULL, name, value)
  //
  // If the prefix is "xml", then the namespace lookup will create it.
  //
  // By contrast, xmlNewProp does not do this parsing and creates an attribute
  // with the name and value exactly as given. This is the behavior that we
  // want.
  //
  // Thus, for attribute names like "xml:lang", #set_attribute will create an
  // attribute with namespace "xml" and name "lang". This is incorrect for
  // html elements (but correct for foreign elements).
  //
  // Work around this by inserting a dummy attribute and then changing the
  // name, if needed.

  // Can't use strchr since it's locale-sensitive.
  size_t len = strlen(name);
  VALUE r_name = rb_str_new(name, len);
  if (memchr(name, ':', len) == NULL) {
    // No colon.
    return rb_funcall(node, set_attribute, 2, r_name, rb_str_new2(value));
  }
  // Find a dummy attribute string that doesn't already exist.
  VALUE dummy = find_dummy_key(node);
  if (dummy == Qnil)
    return Qnil;
  // Add the dummy attribute.
  VALUE r_value = rb_funcall(node, set_attribute, 2, dummy, rb_str_new2(value));
  if (r_value == Qnil)
    return Qnil;
  // Remove thet old attribute, if it exists.
  rb_funcall(node, remove_attribute, 1, r_name);
  // Rename the dummy
  VALUE attr = rb_funcall(node, attribute, 1, dummy);
  if (attr == Qnil)
    return Qnil;
  rb_funcall(attr, node_name_, 1, r_name);
  return attr;
}
#endif

// Build a xmlNodePtr for a given GumboNode (recursively)
static xmlNodePtr walk_tree(xmlDocPtr document, GumboNode *node);

// Build a xmlNodePtr for a given GumboElement (recursively)
static xmlNodePtr walk_element(xmlDocPtr document, GumboElement *node) {
  // create the given element
  xmlNodePtr element = xmlNewDocNode(document, NIL, CONST_CAST node->name, NIL);

  // add in the attributes
  GumboVector* attrs = &node->attributes;
  char *name = NULL;
  size_t namelen = 0;
  const char *ns;
  for (size_t i=0; i < attrs->length; i++) {
    GumboAttribute *attr = attrs->data[i];

    switch (attr->attr_namespace) {
      case GUMBO_ATTR_NAMESPACE_XLINK:
        ns = "xlink:";
        break;

      case GUMBO_ATTR_NAMESPACE_XML:
        ns = "xml:";
        break;

      case GUMBO_ATTR_NAMESPACE_XMLNS:
        ns = "xmlns:";
        if (!strcmp(attr->name, "xmlns")) ns = NULL;
        break;

      default:
        ns = NULL;
    }

    if (ns) {
      if (strlen(ns) + strlen(attr->name) + 1 > namelen) {
        free(name);
        name = NULL;
      }

      if (!name) {
        namelen = strlen(ns) + strlen(attr->name) + 1;
        name = malloc(namelen);
      }

      strcpy(name, ns);
      strcat(name, attr->name);
      xmlNewProp(element, CONST_CAST name, CONST_CAST attr->value);
    } else {
      xmlNewProp(element, CONST_CAST attr->name, CONST_CAST attr->value);
    }
  }
  if (name) free(name);

  // add in the children
  GumboVector* children = &node->children;
  for (size_t i=0; i < children->length; i++) {
    xmlNodePtr node = walk_tree(document, children->data[i]);
    if (node) xmlAddChild(element, node);
  }

  return element;
}

static xmlNodePtr walk_tree(xmlDocPtr document, GumboNode *node) {
  switch (node->type) {
    case GUMBO_NODE_DOCUMENT:
      return NIL;
    case GUMBO_NODE_ELEMENT:
    case GUMBO_NODE_TEMPLATE:
      return walk_element(document, &node->v.element);
    case GUMBO_NODE_TEXT:
    case GUMBO_NODE_WHITESPACE:
      return xmlNewDocText(document, CONST_CAST node->v.text.text);
    case GUMBO_NODE_CDATA:
      return xmlNewCDataBlock(document,
        CONST_CAST node->v.text.text,
        (int) strlen(node->v.text.text));
    case GUMBO_NODE_COMMENT:
      return xmlNewDocComment(document, CONST_CAST node->v.text.text);
  }
}

// URI = system id
// external id = public id
#if NGLIB
static htmlDocPtr new_html_doc(const char *dtd_name, const char *system, const char *public)
{
  // These two libxml2 functions take the public and system ids in
  // opposite orders.
  htmlDocPtr doc = htmlNewDocNoDtD(/* URI */ NULL, /* ExternalID */NULL);
  assert(doc);
  if (dtd_name)
    xmlCreateIntSubset(doc, CONST_CAST dtd_name, CONST_CAST public, CONST_CAST system);
  return doc;
}
#else
// remove internal subset from newly created documents
static VALUE new_html_doc(const char *dtd_name, const char *system, const char *public) {
  VALUE doc;
  // If system and public are both NULL, Document#new is going to set default
  // values for them so we're going to have to remove the internal subset
  // which seems to leak memory in Nokogiri, so leak as little as possible.
  if (system == NULL && public == NULL) {
    doc = rb_funcall(Document, new, 2, /* URI */ Qnil, /* external_id */ rb_str_new("", 0));
    rb_funcall(rb_funcall(doc, internal_subset, 0), remove_, 0);
    if (dtd_name) {
      // We need to create an internal subset now.
      rb_funcall(doc, create_internal_subset, 3, rb_str_new2(dtd_name), Qnil, Qnil);
    }
  } else {
    assert(dtd_name);
    // Rather than removing and creating the internal subset as we did above,
    // just create and then rename one.
    VALUE r_system = system ? rb_str_new2(system) : Qnil;
    VALUE r_public = public ? rb_str_new2(public) : Qnil;
    doc = rb_funcall(Document, new, 2, r_system, r_public);
    rb_funcall(rb_funcall(doc, internal_subset, 0), node_name_, 1, rb_str_new2(dtd_name));
  }
  return doc;
}
#endif

// Parse a string using gumbo_parse into a Nokogiri document
static VALUE parse(VALUE self, VALUE string, VALUE url, VALUE max_errors) {
  GumboOptions options = kGumboDefaultOptions;
  options.max_errors = NUM2INT(max_errors);

  const char *input = RSTRING_PTR(string);
  size_t input_len = RSTRING_LEN(string);
  GumboOutput *output = gumbo_parse_with_options(&options, input, input_len);
  xmlDocPtr doc;
  if (output->document->v.document.has_doctype) {
    const char *name   = output->document->v.document.name;
    const char *public = output->document->v.document.public_identifier;
    const char *system = output->document->v.document.system_identifier;
    public = public[0] ? public : NULL;
    system = system[0] ? system : NULL;
    doc = new_html_doc(name, system, public);
  } else {
    doc = new_html_doc(NULL, NULL, NULL);
  }

  GumboVector *children = &output->document->v.document.children;
  for (size_t i=0; i < children->length; i++) {
    GumboNode *child = children->data[i];
    xmlNodePtr node = walk_tree(doc, child);
    if (node) {
      if (child == output->root)
        xmlDocSetRootElement(doc, node);
      else
        xmlAddChild((xmlNodePtr)doc, node);
    }
  }

  VALUE rdoc = Nokogiri_wrap_xml_document(Document, doc);

  // Add parse errors to rdoc.
  if (output->errors.length) {
    GumboVector *errors = &output->errors;
    GumboStringBuffer msg;
    VALUE rerrors = rb_ary_new2(errors->length);

    gumbo_string_buffer_init(&msg);
    for (size_t i=0; i < errors->length; i++) {
      GumboError *err = errors->data[i];
      gumbo_string_buffer_clear(&msg);
      gumbo_caret_diagnostic_to_string(err, input, input_len, &msg);
      VALUE err_str = rb_str_new(msg.data, msg.length);
      VALUE syntax_error = rb_class_new_instance(1, &err_str, cNokogiriXmlSyntaxError);
      rb_iv_set(syntax_error, "@domain", INT2NUM(1)); // XML_FROM_PARSER
      rb_iv_set(syntax_error, "@code", INT2NUM(1));   // XML_ERR_INTERNAL_ERROR
      rb_iv_set(syntax_error, "@level", INT2NUM(2));  // XML_ERR_ERROR
      rb_iv_set(syntax_error, "@file", url);
      rb_iv_set(syntax_error, "@line", INT2NUM(err->position.line));
      rb_iv_set(syntax_error, "@str1", Qnil);
      rb_iv_set(syntax_error, "@str2", Qnil);
      rb_iv_set(syntax_error, "@str3", Qnil);
      rb_iv_set(syntax_error, "@int1", INT2NUM(err->type));
      rb_iv_set(syntax_error, "@column", INT2NUM(err->position.column));
      rb_ary_push(rerrors, syntax_error);
    }
    rb_iv_set(rdoc, "@errors", rerrors);
    gumbo_string_buffer_destroy(&msg);
  }

  gumbo_destroy_output(output);

  return rdoc;
}

// Initialize the Nokogumbo class and fetch constants we will use later
void Init_nokogumbo() {
  rb_funcall(rb_mKernel, rb_intern("gem"), 1, rb_str_new2("nokogiri"));
  rb_require("nokogiri");

  // class constants
  VALUE Nokogiri = rb_const_get(rb_cObject, rb_intern("Nokogiri"));
  VALUE HTML5 = rb_const_get(Nokogiri, rb_intern("HTML5"));
  Document = rb_const_get(HTML5, rb_intern("Document"));

#ifndef NGLIB
  // more class constants
  VALUE XML = rb_const_get(Nokogiri, rb_intern("XML"));
  cNokogiriXmlSyntaxError = rb_const_get(XML, rb_intern("SyntaxError"));
  Element = rb_const_get(XML, rb_intern("Element"));
  Text = rb_const_get(XML, rb_intern("Text"));
  CDATA = rb_const_get(XML, rb_intern("CDATA"));
  Comment = rb_const_get(XML, rb_intern("Comment"));

  // interned symbols
  new = rb_intern("new");
  attribute = rb_intern("attribute");
  set_attribute = rb_intern("set_attribute");
  remove_attribute = rb_intern("remove_attribute");
  add_child = rb_intern("add_child_node_and_reparent_attrs");
  internal_subset = rb_intern("internal_subset");
  remove_ = rb_intern("remove");
  create_internal_subset = rb_intern("create_internal_subset");
  key_ = rb_intern("key?");
  node_name_ = rb_intern("node_name=");
#endif

  // define Nokogumbo module with a parse method
  VALUE Gumbo = rb_define_module("Nokogumbo");
  rb_define_singleton_method(Gumbo, "parse", parse, 3);
}
