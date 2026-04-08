#include <circle/string.h>
#include <assert.h>
#include "p9chan.h"
#include "p9error.h"
#include <string.h>

extern const unsigned char wlan_bin[];
extern const unsigned long wlan_bin_size;
extern const unsigned char wlan_txt[];
extern const unsigned long wlan_txt_size;
extern const unsigned char wlan_clm[];
extern const unsigned long wlan_clm_size;

static const struct { const char *name; const unsigned char *data; const unsigned long *size; } s_fw[] = {
	{ "brcmfmac43455-sdio.bin",      wlan_bin, &wlan_bin_size },
	{ "brcmfmac43455-sdio.txt",      wlan_txt, &wlan_txt_size },
	{ "brcmfmac43455-sdio.clm_blob", wlan_clm, &wlan_clm_size },
};

static CString *s_pPath = 0;

Chan *namec (const char *name, unsigned func, unsigned flags, unsigned opt)
{
	Chan *c = new Chan;
	assert (c != 0);

	c->type    = 0;
	c->open    = 0;
	c->membuf  = 0;
	c->memsize = 0;
	c->offset  = 0;

	for (unsigned i = 0; i < sizeof(s_fw)/sizeof(s_fw[0]); i++)
	{
		if (strcmp(name, s_fw[i].name) == 0)
		{
			c->membuf  = s_fw[i].data;
			c->memsize = *s_fw[i].size;
			c->open    = 1;
			return c;
		}
	}

	assert (s_pPath != 0);
	CString Path;
	Path.Format ("%s%s", (const char *) *s_pPath, name);

	FRESULT Result = f_open (&c->file, Path, FA_READ | FA_OPEN_EXISTING);
	if (Result != FR_OK)
	{
		delete c;

		print ("File: %s\n", (const char *) Path);
		error (Enonexist);

		return 0;
	}

	c->open = 1;

	return c;
}

void cclose (Chan *c)
{
	assert (c->open);

	if (!c->membuf)
		f_close (&c->file);

	c->open = 0;

	delete c;
}

static int readchan (Chan *c, void *buf, size_t len, ulong offset)
{
	assert (c->open);

	if (c->membuf)
	{
		if (offset >= c->memsize) return 0;
		if (offset + len > c->memsize) len = c->memsize - offset;
		memcpy(buf, c->membuf + offset, len);
		return (int) len;
	}

	FRESULT Result;
	if (c->offset != offset)
	{
		Result = f_lseek (&c->file, offset);
		assert (Result == FR_OK);

		c->offset = offset;
	}

	unsigned nBytesRead;
	Result = f_read (&c->file, buf, len, &nBytesRead);
	if (Result != FR_OK)
	{
		error (Eio);

		return -1;
	}

	c->offset += nBytesRead;

	return (int) nBytesRead;
}

static struct device_t devchan
{
	readchan
};

struct device_t *devtab[1] =
{
	&devchan
};

void p9chan_init (const char *path)
{
	assert (s_pPath == 0);
	s_pPath = new CString (path);
	assert (s_pPath != 0);
}
