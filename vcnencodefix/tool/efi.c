// efi.c - adds or removes VCNEncodeFloor.kext in OpenCore's config.plist
// (Kernel > Add).
//
// The file is edited as text so that the only change is the entry itself (the
// rest of the file keeps its exact bytes and formatting). The result is then
// parsed with CoreFoundation and compared with what it must be (the original
// property list plus, or minus, the entry). If the text edit does not give
// exactly that, the property list is written by CoreFoundation instead, which
// keeps every value but reformats the file. A timestamped backup of the
// original is written next to it before anything changes, and the new file
// replaces the old one only after it has been fully written.
#include "efi.h"

#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char* kBundle = "VCNEncodeFloor.kext";
// Darwin 23 = Sonoma, the oldest macOS the kext supports (its Lilu
// configuration allows Sonoma to Tahoe). 1.0.0 to 1.2.0 wrote MinKernel 25.0.0
// (Tahoe only), so OpenCore skipped the kext on Sonoma and Sequoia; efi-add
// updates such an entry in place.
#define MIN_KERNEL "23.0.0"
#define OLD_MIN_KERNEL "25.0.0"

typedef struct {
  char* p;
  size_t n;
} Buf;

static int read_file(const char* path, Buf* b) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return -1;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > 16 * 1024 * 1024) {
    fclose(f);
    fprintf(stderr, "%s: unexpected size\n", path);
    return -1;
  }
  b->p = malloc((size_t)n + 1);
  if (!b->p) {
    fclose(f);
    fprintf(stderr, "%s: out of memory\n", path);
    return -1;
  }
  b->n = fread(b->p, 1, (size_t)n, f);
  b->p[b->n] = 0;
  fclose(f);
  return b->n == (size_t)n ? 0 : -1;
}

static int write_file(const char* path, const void* p, size_t n) {
  char tmp[1100];
  snprintf(tmp, sizeof(tmp), "%s.vcnencodefix-tmp", path);
  FILE* f = fopen(tmp, "wb");
  if (!f) {
    perror(tmp);
    return -1;
  }
  size_t w = fwrite(p, 1, n, f);
  int rc = fflush(f) | fsync(fileno(f));
  fclose(f);
  if (w != n || rc != 0) {
    fprintf(stderr, "%s: write failed\n", tmp);
    unlink(tmp);
    return -1;
  }
  if (rename(tmp, path) != 0) {
    perror("rename");
    unlink(tmp);
    return -1;
  }
  return 0;
}

static CFPropertyListRef parse(const void* p, size_t n,
                               CFPropertyListFormat* fmt) {
  CFDataRef d = CFDataCreate(NULL, p, (CFIndex)n);
  CFPropertyListRef pl = CFPropertyListCreateWithData(
      NULL, d, kCFPropertyListMutableContainersAndLeaves, fmt, NULL);
  CFRelease(d);
  return pl;
}

static bool str_eq(CFTypeRef v, const char* s) {
  if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return false;
  char buf[256];
  return CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8) &&
         !strcmp(buf, s);
}

static CFMutableDictionaryRef make_entry(void) {
  CFMutableDictionaryRef d =
      CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(d, CFSTR("Arch"), CFSTR("x86_64"));
  CFDictionarySetValue(d, CFSTR("BundlePath"), CFSTR("VCNEncodeFloor.kext"));
  CFDictionarySetValue(d, CFSTR("Comment"),
                       CFSTR("VCNEncodeFix: VCN encode clock floor"));
  CFDictionarySetValue(d, CFSTR("Enabled"), kCFBooleanTrue);
  CFDictionarySetValue(d, CFSTR("ExecutablePath"),
                       CFSTR("Contents/MacOS/VCNEncodeFloor"));
  CFDictionarySetValue(d, CFSTR("MaxKernel"), CFSTR(""));
  CFDictionarySetValue(d, CFSTR("MinKernel"), CFSTR(MIN_KERNEL));
  CFDictionarySetValue(d, CFSTR("PlistPath"), CFSTR("Contents/Info.plist"));
  return d;
}

// Same entry as ours, ignoring the free-text Comment.
static bool same_entry(CFTypeRef e) {
  if (!e || CFGetTypeID(e) != CFDictionaryGetTypeID()) return false;
  CFMutableDictionaryRef a = CFDictionaryCreateMutableCopy(NULL, 0, e),
                         b = make_entry();
  CFDictionaryRemoveValue(a, CFSTR("Comment"));
  CFDictionaryRemoveValue(b, CFSTR("Comment"));
  bool same = CFEqual(a, b);
  CFRelease(a);
  CFRelease(b);
  return same;
}
// The entry written by 1.0.0 to 1.2.0: ours in every setting except
// MinKernel 25.0.0.
static bool old_entry(CFTypeRef e) {
  if (!e || CFGetTypeID(e) != CFDictionaryGetTypeID() ||
      !str_eq(CFDictionaryGetValue(e, CFSTR("MinKernel")), OLD_MIN_KERNEL))
    return false;
  CFMutableDictionaryRef a = CFDictionaryCreateMutableCopy(NULL, 0, e);
  CFDictionarySetValue(a, CFSTR("MinKernel"), CFSTR(MIN_KERNEL));
  bool same = same_entry(a);
  CFRelease(a);
  return same;
}
static const char* entry_lines[][2] = {
    {"Arch", "<string>x86_64</string>"},
    {"BundlePath", "<string>VCNEncodeFloor.kext</string>"},
    {"Comment", "<string>VCNEncodeFix: VCN encode clock floor</string>"},
    {"Enabled", "<true/>"},
    {"ExecutablePath", "<string>Contents/MacOS/VCNEncodeFloor</string>"},
    {"MaxKernel", "<string></string>"},
    {"MinKernel", "<string>" MIN_KERNEL "</string>"},
    {"PlistPath", "<string>Contents/Info.plist</string>"},
};

typedef struct {
  CFMutableDictionaryRef root;
  CFMutableArrayRef add;
  CFIndex lilu, ours, count;
  bool liluEnabled;
} Config;

static int load_config(CFPropertyListRef pl, CFPropertyListFormat fmt,
                       Config* c) {
  memset(c, 0, sizeof(*c));
  c->lilu = c->ours = -1;
  if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) {
    fprintf(stderr, "not a property list dictionary\n");
    return -1;
  }
  if (fmt != kCFPropertyListXMLFormat_v1_0) {
    fprintf(stderr, "config.plist is not XML; OpenCore needs XML\n");
    return -1;
  }
  c->root = (CFMutableDictionaryRef)pl;
  CFTypeRef kernel = CFDictionaryGetValue(c->root, CFSTR("Kernel"));
  if (!kernel || CFGetTypeID(kernel) != CFDictionaryGetTypeID()) {
    fprintf(stderr,
            "no Kernel dictionary: is this an OpenCore config.plist?\n");
    return -1;
  }
  CFTypeRef add = CFDictionaryGetValue(kernel, CFSTR("Add"));
  if (!add || CFGetTypeID(add) != CFArrayGetTypeID()) {
    fprintf(stderr, "no Kernel > Add array\n");
    return -1;
  }
  c->add = (CFMutableArrayRef)add;
  c->count = CFArrayGetCount(c->add);
  for (CFIndex i = 0; i < c->count; i++) {
    CFTypeRef e = CFArrayGetValueAtIndex(c->add, i);
    if (CFGetTypeID(e) != CFDictionaryGetTypeID()) {
      fprintf(stderr, "Kernel > Add item %ld is not a dictionary; refusing\n",
              (long)i);
      return -1;
    }
    CFTypeRef bp = CFDictionaryGetValue(e, CFSTR("BundlePath"));
    if (str_eq(bp, "Lilu.kext") && c->lilu < 0) {
      c->lilu = i;
      c->liluEnabled =
          CFDictionaryGetValue(e, CFSTR("Enabled")) == kCFBooleanTrue;
    }
    if (str_eq(bp, kBundle)) {
      if (c->ours >= 0) {
        fprintf(stderr, "%s is listed twice; fix it by hand\n", kBundle);
        return -1;
      }
      c->ours = i;
    }
  }
  return 0;
}

static const char* find_from(const char* hay, const char* end,
                             const char* needle) {
  size_t n = strlen(needle);
  for (const char* p = hay; p && p + n <= end; p++)
    if (*p == *needle && !memcmp(p, needle, n)) return p;
  return NULL;
}

static const char* line_start(const char* base, const char* p) {
  while (p > base && p[-1] != '\n') p--;
  return p;
}

static bool only_space(const char* a, const char* b) {
  for (; a < b; a++)
    if (*a != ' ' && *a != '\t') return false;
  return true;
}

// Text insertion of our entry before the closing </array> of Kernel > Add.
// Returns a malloc'ed buffer or NULL.
static char* text_add(const Buf* in, size_t* outLen) {
  const char *base = in->p, *end = in->p + in->n;
  const char* k = find_from(base, end, "<key>Kernel</key>");
  const char* a = k ? find_from(k, end, "<key>Add</key>") : NULL;
  if (!a) return NULL;
  // the value of the Add key must be an <array> (only whitespace in between)
  const char* open = a + strlen("<key>Add</key>");
  while (open < end &&
         (*open == ' ' || *open == '\t' || *open == '\n' || *open == '\r'))
    open++;
  if (end - open < 7 || strncmp(open, "<array>", 7) != 0) return NULL;
  int depth = 1;
  const char *p = open + strlen("<array>"), *close = NULL;
  while (p < end && depth > 0) {
    const char* lt = memchr(p, '<', (size_t)(end - p));
    if (!lt) return NULL;
    if (!strncmp(lt, "<array>", 7))
      depth++;
    else if (!strncmp(lt, "</array>", 8)) {
      if (--depth == 0) {
        close = lt;
        break;
      }
    }
    p = lt + 1;
  }
  if (!close) return NULL;
  const char* ls = line_start(base, close);
  if (!only_space(ls, close)) return NULL;
  size_t closeIndentLen = (size_t)(close - ls);
  char entryIndent[64], unit[16] = "\t";
  const char* firstDict = find_from(open, close, "<dict>");
  if (firstDict) {
    const char* ds = line_start(base, firstDict);
    size_t n = (size_t)(firstDict - ds);
    if (!only_space(ds, firstDict) || n >= sizeof(entryIndent)) return NULL;
    memcpy(entryIndent, ds, n);
    entryIndent[n] = 0;
    if (n > closeIndentLen && !memcmp(entryIndent, ls, closeIndentLen) &&
        n - closeIndentLen < sizeof(unit)) {
      memcpy(unit, entryIndent + closeIndentLen, n - closeIndentLen);
      unit[n - closeIndentLen] = 0;
    }
  } else {
    if (closeIndentLen + 1 >= sizeof(entryIndent)) return NULL;
    memcpy(entryIndent, ls, closeIndentLen);
    entryIndent[closeIndentLen] = 0;
    strlcat(entryIndent, ls[0] == ' ' ? "    " : "\t", sizeof(entryIndent));
    if (ls[0] == ' ') strlcpy(unit, "    ", sizeof(unit));
  }
  char entry[2048];
  int len = snprintf(entry, sizeof(entry), "%s<dict>\n", entryIndent);
  if (len < 0 || (size_t)len >= sizeof(entry)) return NULL;
  for (size_t i = 0; i < sizeof(entry_lines) / sizeof(entry_lines[0]); i++) {
    int n = snprintf(entry + len, sizeof(entry) - (size_t)len,
                     "%s%s<key>%s</key>\n%s%s%s\n", entryIndent, unit,
                     entry_lines[i][0], entryIndent, unit, entry_lines[i][1]);
    if (n < 0 || (size_t)n >= sizeof(entry) - (size_t)len) return NULL;
    len += n;
  }
  {
    int n = snprintf(entry + len, sizeof(entry) - (size_t)len, "%s</dict>\n",
                     entryIndent);
    if (n < 0 || (size_t)n >= sizeof(entry) - (size_t)len) return NULL;
    len += n;
  }
  size_t pre = (size_t)(ls - base);
  char* out = malloc(in->n + (size_t)len + 1);
  if (!out) return NULL;
  memcpy(out, base, pre);
  memcpy(out + pre, entry, (size_t)len);
  memcpy(out + pre + len, base + pre, in->n - pre);
  *outLen = in->n + (size_t)len;
  out[*outLen] = 0;
  return out;
}

// Text removal of the <dict> ... </dict> lines that hold our BundlePath.
// Returns a malloc'ed buffer or NULL.
static char* text_remove(const Buf* in, size_t* outLen) {
  const char *base = in->p, *end = in->p + in->n;
  const char* m = find_from(base, end, "<string>VCNEncodeFloor.kext</string>");
  if (!m || find_from(m + 1, end, "<string>VCNEncodeFloor.kext</string>"))
    return NULL;
  const char* d = NULL;
  for (const char* p = m; p >= base + 6; p--)
    if (!strncmp(p, "<dict>", 6)) {
      d = p;
      break;
    }
  const char* e = d ? find_from(m, end, "</dict>") : NULL;
  if (!d || !e || find_from(d + 6, m, "<dict>") || find_from(d, m, "</dict>"))
    return NULL;
  const char* from = line_start(base, d);
  if (!only_space(from, d)) return NULL;
  const char* to = e + strlen("</dict>");
  if (to < end && *to == '\n')
    to++;
  else
    return NULL;
  size_t pre = (size_t)(from - base), post = (size_t)(end - to);
  char* out = malloc(pre + post + 1);
  if (!out) return NULL;
  memcpy(out, base, pre);
  memcpy(out + pre, to, post);
  *outLen = pre + post;
  out[*outLen] = 0;
  return out;
}

// Text change of MinKernel 25.0.0 to 23.0.0 inside the <dict> that holds our
// BundlePath. Returns a malloc'ed buffer or NULL.
static char* text_minkernel(const Buf* in, size_t* outLen) {
  const char *base = in->p, *end = in->p + in->n;
  const char* m = find_from(base, end, "<string>VCNEncodeFloor.kext</string>");
  if (!m || find_from(m + 1, end, "<string>VCNEncodeFloor.kext</string>"))
    return NULL;
  const char* d = NULL;
  for (const char* p = m; p >= base + 6; p--)
    if (!strncmp(p, "<dict>", 6)) {
      d = p;
      break;
    }
  const char* e = d ? find_from(m, end, "</dict>") : NULL;
  if (!d || !e || find_from(d + 6, e, "<dict>")) return NULL;
  const char* k = find_from(d, e, "<key>MinKernel</key>");
  if (!k || find_from(k + 1, e, "<key>MinKernel</key>")) return NULL;
  const char* v = k + strlen("<key>MinKernel</key>");
  while (v < e && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
  const char *oldv = "<string>" OLD_MIN_KERNEL "</string>",
             *newv = "<string>" MIN_KERNEL "</string>";
  size_t on = strlen(oldv), nn = strlen(newv);
  if ((size_t)(e - v) < on || memcmp(v, oldv, on) != 0) return NULL;
  size_t pre = (size_t)(v - base), post = (size_t)(end - (v + on));
  char* out = malloc(pre + nn + post + 1);
  if (!out) return NULL;
  memcpy(out, base, pre);
  memcpy(out + pre, newv, nn);
  memcpy(out + pre + nn, v + on, post);
  *outLen = pre + nn + post;
  out[*outLen] = 0;
  return out;
}
static int backup(const char* path, const Buf* orig, char* out, size_t outLen) {
  char ts[32];
  time_t t = time(NULL);
  strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", localtime(&t));
  snprintf(out, outLen, "%s.backup-%s-pre-vcnencodefix", path, ts);
  for (int i = 2; access(out, F_OK) == 0 && i < 100; i++)
    snprintf(out, outLen, "%s.backup-%s-%d-pre-vcnencodefix", path, ts, i);
  FILE* f = fopen(out, "wbx");
  if (!f) {
    perror(out);
    return -1;
  }
  size_t w = fwrite(orig->p, 1, orig->n, f);
  int rc = fflush(f) | fsync(fileno(f));
  fclose(f);
  return (w == orig->n && rc == 0) ? 0 : -1;
}

int efi_command(const char* what, const char* path) {
  Buf orig = {0};
  if (read_file(path, &orig) != 0) return 1;
  CFPropertyListFormat fmt = 0;
  CFPropertyListRef pl = parse(orig.p, orig.n, &fmt);
  Config c;
  if (load_config(pl, fmt, &c) != 0) return 1;

  if (!strcmp(what, "check")) {
    printf("%s: Kernel > Add has %ld entries; Lilu.kext %s; %s %s\n", path,
           (long)c.count,
           c.lilu < 0      ? "missing"
           : c.liluEnabled ? "enabled"
                           : "disabled",
           kBundle, c.ours < 0 ? "not listed" : "listed");
    if (c.ours >= 0) {
      CFTypeRef e = CFArrayGetValueAtIndex(c.add, c.ours);
      bool en = CFDictionaryGetValue(e, CFSTR("Enabled")) == kCFBooleanTrue;
      printf(
          "  entry %ld, %s, %s, %s\n", (long)c.ours,
          en ? "enabled" : "DISABLED",
          c.ours > c.lilu ? "after Lilu" : "BEFORE Lilu (must come after it)",
          same_entry(e)  ? "settings as expected"
          : old_entry(e) ? "old entry with MinKernel " OLD_MIN_KERNEL
                           " (Tahoe only): efi-add updates it"
                         : "settings differ from the expected entry");
    }
    return 0;
  }

  CFMutableDictionaryRef expected =
      (CFMutableDictionaryRef)CFPropertyListCreateDeepCopy(
          NULL, pl, kCFPropertyListMutableContainersAndLeaves);
  Config ce;
  load_config(expected, fmt, &ce);
  char* text = NULL;
  size_t textLen = 0;
  const char* done = !strcmp(what, "add") ? "added" : "removed";
  const char* verified = !strcmp(what, "add")
                             ? "equals the original plus the entry"
                             : "equals the original minus the entry";
  if (!strcmp(what, "add")) {
    if (c.lilu < 0 || !c.liluEnabled) {
      fprintf(stderr,
              "Lilu.kext is not enabled in Kernel > Add; VCNEncodeFloor needs "
              "it\n");
      return 1;
    }
    if (c.ours >= 0) {
      CFTypeRef cur = CFArrayGetValueAtIndex(c.add, c.ours);
      bool same = same_entry(cur);
      if (same && c.ours > c.lilu) {
        printf("%s is already in Kernel > Add (entry %ld); nothing to change\n",
               kBundle, (long)c.ours);
        return 0;
      }
      if (old_entry(cur) && c.ours > c.lilu) {
        // entry from 1.0.0 to 1.2.0: only MinKernel changes
        CFMutableDictionaryRef e =
            (CFMutableDictionaryRef)CFArrayGetValueAtIndex(ce.add, c.ours);
        CFDictionarySetValue(e, CFSTR("MinKernel"), CFSTR(MIN_KERNEL));
        text = text_minkernel(&orig, &textLen);
        done = "updated MinKernel " OLD_MIN_KERNEL " -> " MIN_KERNEL " of";
        verified = "equals the original except for that MinKernel";
      } else {
        fprintf(stderr,
                "%s is listed but differs from the expected entry (or comes "
                "before Lilu); run efi-remove first\n",
                kBundle);
        return 1;
      }
    } else {
      CFMutableDictionaryRef e = make_entry();
      CFArrayAppendValue(ce.add, e);
      CFRelease(e);
      text = text_add(&orig, &textLen);
    }
  } else if (!strcmp(what, "remove")) {
    if (c.ours < 0) {
      printf("%s is not in Kernel > Add; nothing to change\n", kBundle);
      return 0;
    }
    CFArrayRemoveValueAtIndex(ce.add, c.ours);
    text = text_remove(&orig, &textLen);
  } else
    return 2;

  const void* outP = NULL;
  size_t outN = 0;
  CFDataRef cfOut = NULL;
  if (text) {
    CFPropertyListFormat f2;
    CFPropertyListRef got = parse(text, textLen, &f2);
    if (got && f2 == kCFPropertyListXMLFormat_v1_0 && CFEqual(got, expected)) {
      outP = text;
      outN = textLen;
    }
    if (got) CFRelease(got);
  }
  const char* how = "text edit, only the entry changes";
  if (!outP) {
    cfOut = CFPropertyListCreateData(NULL, expected,
                                     kCFPropertyListXMLFormat_v1_0, 0, NULL);
    if (!cfOut) {
      fprintf(stderr, "could not serialize the new property list\n");
      return 1;
    }
    outP = CFDataGetBytePtr(cfOut);
    outN = (size_t)CFDataGetLength(cfOut);
    how = "rewritten by CoreFoundation (same values, new formatting)";
  }
  char bak[1200];
  if (backup(path, &orig, bak, sizeof(bak)) != 0) {
    fprintf(stderr, "backup failed; nothing changed\n");
    return 1;
  }
  if (write_file(path, outP, outN) != 0) {
    fprintf(stderr, "write failed; the original is unchanged (backup %s)\n",
            bak);
    return 1;
  }

  // verify what is on disk now
  Buf now = {0};
  CFPropertyListFormat f3;
  CFPropertyListRef check =
      read_file(path, &now) == 0 ? parse(now.p, now.n, &f3) : NULL;
  bool ok = check && CFEqual(check, expected);
  printf("%s: %s %s (%s)\n  backup: %s\n", path, done, kBundle, how, bak);
  if (!ok) {
    fprintf(stderr,
            "VERIFY FAILED: restore the backup above before rebooting\n");
    return 1;
  }
  printf("  verified: the file parses and %s\n", verified);
  return 0;
}
