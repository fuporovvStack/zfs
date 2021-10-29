/*
 * COMMENT FROM ORIGINAL VERSION:
Originally written by Jeff Bonwick
(http://www.mail-archive.com/zfs-discuss@opensolaris.org/msg15748.html),
and updated by James Lee to work with modern ZFS libs
(https://www.mail-archive.com/zfs-discuss@opensolaris.org/msg47316.html),
I was able to compile this life-saving utility in Ubuntu 14.04 and have verified
that it works. (I'm using ZFSonLinux.) Download the ZFSonLinux tarball and
replace the cmd/zhack/zhack.c file with "labelfix.c". Note that zhack is just a
simple utility that we're replacing so that we don't have to setup the build
environment. (It's hard, so we'll reuse the good work of the ZFSonLinux people.)
Run "./configure; make" and if all goes well then the zfs tools will be built,
except for zhack, which we replaced. Run that with the device path to recover
your data. If you want to be super-careful and not tamper with your disk,
you can clone it and run the utility on your clone. Or, create an overlay as
described in this page:
https://raid.wiki.kernel.org/index.php/Recovering_a_failed_software_RAID#Making_the_harddisks_read-only_using_an_overlay_file
*/

/*
 * HOW TO BUILD:
 * - copy/paste this file content to zhack.c
 * - cd zfs && ./autogen.sh ; ./configure ; make ; sudo make install
 * - cp $(which zhack) name_you_wish_this_utility_will_be_called
 */

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stddef.h>

#include <sys/zio_checksum.h>
#include <sys/vdev_impl.h>

#include <libnvpair.h>

static uint64_t
label_get_offset(uint64_t psize, int l)
{
	return (l * sizeof (vdev_label_t) + (l < VDEV_LABELS / 2 ?
	    0 : psize - VDEV_LABELS * sizeof (vdev_label_t)));
}

static boolean_t
label_dump_csum(int l, vdev_label_t *vl, uint64_t label_offset)
{
	zio_checksum_info_t *ci = &zio_checksum_table[ZIO_CHECKSUM_LABEL];
	zio_cksum_t expected_cksum;
	zio_cksum_t actual_cksum;
	zio_cksum_t verifier;
	zio_eck_t *eck;
	uint64_t size = VDEV_PHYS_SIZE;
	int byteswap;

	void *data = (char *)vl + offsetof(vdev_label_t, vl_vdev_phys);
	eck = (zio_eck_t *)((char *)(data) + size) - 1;

	if ((eck->zec_magic != ZEC_MAGIC) &&
	    (eck->zec_magic != BSWAP_64(ZEC_MAGIC)))
		ASSERT(!"Bad csum magic on label read");

	uint64_t offset = label_offset + offsetof(vdev_label_t, vl_vdev_phys);
	ZIO_SET_CHECKSUM(&verifier, offset, 0, 0, 0);

	byteswap = (eck->zec_magic == BSWAP_64(ZEC_MAGIC));
	if (byteswap)
		byteswap_uint64_array(&verifier, sizeof (zio_cksum_t));

	expected_cksum = eck->zec_cksum;
	eck->zec_cksum = verifier;

	abd_t *abd = abd_get_from_buf(data, size);
	ci->ci_func[byteswap](abd, size, NULL, &actual_cksum);

	if (byteswap)
		byteswap_uint64_array(&expected_cksum,
		    sizeof (zio_cksum_t));

	if (ZIO_CHECKSUM_EQUAL(actual_cksum, expected_cksum))
		return (B_TRUE);

	return (B_FALSE);
}


static uint64_t
label_read(const char *path, int l, vdev_label_t *vl)
{
	int fd;
	struct stat st;
	uint64_t psize, offset;

	VERIFY((fd = open(path, O_RDWR)) != -1);
	VERIFY(stat(path, &st) == 0);
	psize = st.st_size;
	offset = label_get_offset(psize, l);

	VERIFY(pread64(fd, vl, sizeof (vdev_label_t), offset) ==
	    sizeof (vdev_label_t));

	close(fd);

	return (offset);
}

static uint64_t
label_mask(const char *arg)
{
	int mask = 0;

	for (int i = 0; i < VDEV_LABELS; i++)
		if (arg[i] != '0')
			mask |= 1 << i;

	return (mask);
}

static void
label_dump(vdev_label_t *vl)
{
	nvlist_t *config;

	VERIFY(nvlist_unpack(vl->vl_vdev_phys.vp_nvlist,
	    sizeof (vl->vl_vdev_phys.vp_nvlist), &config, 0) == 0);

	dump_nvlist(config, 8);
}

static void
label_update_int_key(nvlist_t *config, const char *key, uint64_t val)
{
	uint64_t ret;
	int err = nvlist_lookup_uint64(config, key, &ret);
	if (err == 0) {
		VERIFY(nvlist_remove_all(config, key) == 0);
		VERIFY(nvlist_add_uint64(config, key, val) == 0);
	} else {
		VERIFY(nvlist_add_uint64(config, key, val) == 0);
	}
}

static void
label_update_string_key(nvlist_t *config, const char *key, const char *val)
{
	char *ret;
	int err = nvlist_lookup_string(config, key, &ret);
	if (err == 0) {
		VERIFY(nvlist_remove_all(config, key) == 0);
		VERIFY(nvlist_add_string(config, key, val) == 0);
	} else {
		VERIFY(nvlist_add_string(config, key, val) == 0);
	}
}

static void
label_write(const char *path, int l, vdev_label_t *vl,
    boolean_t randomize_csum, boolean_t zero_csum)
{
	zio_checksum_info_t *ci = &zio_checksum_table[ZIO_CHECKSUM_LABEL];
	int fd;
	struct stat st;
	uint64_t psize, offset;
	zio_eck_t *eck;
	zio_cksum_t zc;
	abd_t *abd = NULL;
	uint64_t size = VDEV_PHYS_SIZE;
	int byteswap;

	VERIFY((fd = open(path, O_RDWR)) != -1);
	VERIFY(stat(path, &st) == 0);
	psize = st.st_size;
	offset = label_get_offset(psize, l);

	void *data = (char *)vl + offsetof(vdev_label_t, vl_vdev_phys);
	offset = offset + offsetof(vdev_label_t, vl_vdev_phys);
	eck = (zio_eck_t *)((char *)(data) + size) - 1;

	if ((eck->zec_magic != ZEC_MAGIC) &&
	    (eck->zec_magic != BSWAP_64(ZEC_MAGIC)))
		ASSERT(!"Bad csum magic on label write");

	if (randomize_csum) {
		ZIO_SET_CHECKSUM(&eck->zec_cksum, rand(), rand(), rand(),
		    rand());
	} else if (zero_csum) {
		ZIO_SET_CHECKSUM(&eck->zec_cksum, 0, 0, 0, 0);
	} else {
		byteswap = (eck->zec_magic == BSWAP_64(ZEC_MAGIC));

		abd = abd_get_from_buf(data, size);
		ci->ci_func[byteswap](abd, size, NULL, &zc);
		if (byteswap)
			byteswap_uint64_array(&zc, sizeof (zio_cksum_t));

		eck->zec_cksum = zc;
	}

	VERIFY(pwrite64(fd, data, size, offset) == size);

	fsync(fd);
	close(fd);
}


static void
usage(void)
{
	printf("ZFS device labels modification utility.\n");
	printf("CLI options:\n");
	printf("-l: labels mask to process.\n");
	printf("    Example: -l 0101, to modify second and last labels\n");
	printf("-r: randomize requested labels csum.\n");
	printf("-z: zero reqested labels csum.\n");
	printf("-k -i/-s: add/update label config values.\n");
	printf("    Example: '-k pool_guid -i 100500' or '-k name -s test'\n");
	printf("-d: delete lavel config values.\n");
	printf("    Example: '-k pool_guid -d\n");
	printf("Usage example:\n");
	printf("    util -l 1000 /path/to/zfs/dev\n");

	exit(0);
}

int
main(int argc, char **argv)
{
	int c;
	int randomize_csum = 0;
	int zero_csum = 0;
	char *dev = NULL, *resolved_dev = NULL;
	char resolved_path[PATH_MAX];
	char *key = NULL, *val_str = NULL;
	uint64_t val_int = 0;
	boolean_t delete_key = B_FALSE;
	uint64_t labels_mask = 0;
	vdev_label_t labels[4] = {0};
	nvlist_t *configs[VDEV_LABELS];

	dev = argv[argc-1];

	while ((c = getopt (argc, argv, "dhi:k:l:rs:z")) != -1) {
		switch (c)
		{
			case 'd':
				delete_key = B_TRUE;
				break;
			case 'h':
				usage();
				break;
			case 'i':
				val_int = atoll(optarg);
				break;
			case 'k':
				key = malloc(PATH_MAX);
				strcpy(key, optarg);
				break;
			case 'l':
				labels_mask = label_mask(optarg);
				break;
			case 'r':
				randomize_csum = 1;
				break;
			case 's':
				val_str = malloc(PATH_MAX);
				strcpy(val_str, optarg);
				break;
			case 'z':
				zero_csum = 1;
				break;
			case '?':
			default:
				usage();
		}
	}

	/*
	 * Arguments validation.
	 */
	resolved_dev = realpath(dev, resolved_path);
	if (resolved_dev == NULL) {
		printf("Cannot get access to zfs device: %s\n", dev);
		exit(1);
	}

	if (labels_mask == 0) {
		printf("No zfs device labels to process\n");
		exit(1);
	}

	if (randomize_csum && zero_csum) {
		printf("Cannot both randomize and zero csum\n");
		exit(1);
	}

	abd_init();

	for (int i = 0; i < VDEV_LABELS; i++) {
		if ((labels_mask & (1 << i)) == 0)
			continue;

		uint64_t label_offset = label_read(dev, i, &labels[i]);

		if(label_dump_csum(i, &labels[i], label_offset))
			printf("==== label: %d\n", i);
		else
			printf("==== label: %d -> BAD csum\n", i);

		label_dump(&labels[i]);
	}

	if (key) {
		for (int i = 0; i < VDEV_LABELS; i++) {
			if ((labels_mask & (1 << i)) == 0)
				continue;

			VERIFY(nvlist_unpack(labels[i].vl_vdev_phys.vp_nvlist,
			    sizeof (labels[i].vl_vdev_phys.vp_nvlist),
			    &configs[i], 0) == 0);

			if (delete_key) {
				VERIFY(nvlist_remove_all(configs[i], key) == 0);
			} else {
				if (val_str == NULL)
					label_update_int_key(configs[i], key,
					    val_int);
				else
					label_update_string_key(configs[i], key,
					    val_str);
			}

			char *buf = labels[i].vl_vdev_phys.vp_nvlist;
			uint64_t buflen = sizeof (labels[i].vl_vdev_phys.vp_nvlist);
			VERIFY(nvlist_pack(configs[i], &buf, &buflen,
			    NV_ENCODE_XDR, 0) == 0);
		}
	}

	if (key == NULL && randomize_csum == 0 && zero_csum == 0)
		goto out;

	for (int i = 0; i < VDEV_LABELS; i++) {
		if (labels_mask & (1 << i)) {
			label_write(dev, i, &labels[i], randomize_csum, zero_csum);
		}
	}

out:
	abd_fini();

	return (0);
}

