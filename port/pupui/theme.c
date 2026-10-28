/*
 * pupui themes: replacement pictures and names, kept out of the repo and the release.
 *
 * scripts/theme-push.sh converts your pictures to the files below and puts them in
 * Android's /data/linux/theme; the initramfs copies that folder to THEME_DIR at boot.
 * Each file is optional; anything missing or malformed keeps the built-in art.
 *
 *   pupsmall0.raw .. pupsmall5.raw   96x96    the pups (home tiles, Pup Pop, Parade)
 *   badge0.raw .. badge5.raw         100x100  Match cards
 *   home.raw                         640x480  home screen background
 *   names.txt                        up to 6 lines, the Parade names
 *
 * .raw: "RFI1", u32 width, u32 height (little-endian), then width*height ARGB8888 pixels.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

#define THEME_DIR "/var/lib/rufus/theme"

static const struct image *load_raw(const char *name, int w, int h)
{
	char path[128];
	unsigned char hdr[12];
	FILE *f;

	snprintf(path, sizeof(path), THEME_DIR "/%s", name);
	f = fopen(path, "rb");
	if (!f)
		return NULL;

	struct image *im = NULL;
	uint32_t *px = malloc((size_t)w * h * 4);
	if (px && fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "RFI1", 4) &&
	    (hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24) == (uint32_t)w &&
	    (hdr[8] | hdr[9] << 8 | hdr[10] << 16 | (uint32_t)hdr[11] << 24) == (uint32_t)h &&
	    fread(px, 4, (size_t)w * h, f) == (size_t)w * h && fgetc(f) == EOF &&
	    (im = malloc(sizeof(*im)))) {
		im->w = w;
		im->h = h;
		im->px = px;	/* little-endian file, little-endian CPU */
	} else {
		fprintf(stderr, "theme: ignoring %s (expected %dx%d)\n", path, w, h);
		free(px);
	}
	fclose(f);
	return im;
}

void theme_load(void)
{
	const struct image *im;
	char name[32];

	for (int i = 0; i < 6; i++) {
		snprintf(name, sizeof(name), "pupsmall%d.raw", i);
		if ((im = load_raw(name, 96, 96)))
			pups_small[i] = im;
		snprintf(name, sizeof(name), "badge%d.raw", i);
		if ((im = load_raw(name, 100, 100)))
			badges[i] = im;
	}
	if ((im = load_raw("home.raw", W, H)))
		home_img = im;

	FILE *f = fopen(THEME_DIR "/names.txt", "r");
	char line[64];
	for (int i = 0; f && i < 6 && fgets(line, sizeof(line), f); i++) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0])
			pup_names[i] = strdup(line);
	}
	if (f)
		fclose(f);
}
