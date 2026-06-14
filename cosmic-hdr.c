/*
 * cosmic-hdr.c  — HDR10 + BT.2020/DCI-P3 color pipeline injector via KMS atomic
 *
 * Mirrors KDE Plasma 6 HDR pipeline:
 *   DEGAMMA (sRGB→linear) → CTM (BT.709→target gamut) → GAMMA (linear→PQ/ST2084)
 *   + HDR_OUTPUT_METADATA + Colorspace=BT2020_RGB on the connector
 *
 * Steals DRM master via VT switch (tty1→tty2→tty1, screen blanks ~0.5s).
 * Properties persist after master release (cosmic-comp doesn't reset them).
 *
 * Usage (must be root):
 *   sudo cosmic-hdr                          apply (reads /etc/cosmic-hdr.conf)
 *   sudo cosmic-hdr reset                    restore SDR
 *   sudo cosmic-hdr --sdr-nits 203          override SDR white brightness
 *   sudo cosmic-hdr --peak-nits 800         override display peak
 *   sudo cosmic-hdr --gamut 100             override gamut expansion 0-100%
 *   sudo cosmic-hdr --gamut-mode dci-p3     use DCI-P3 D65 instead of BT.2020
 *   sudo cosmic-hdr --bpc 10               request 10-bit output
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/vt.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>

/* ── config ──────────────────────────────────────────────────────────────── */
#define DRM_DEV    "/dev/dri/card1"
#define CONF_PATH  "/etc/cosmic-hdr.conf"
#define LUT_SIZE   4096

/*
 * SDR_NITS: brightness of SDR white in HDR mode (nits).
 *   KDE default: 200. Standard reference: 203 (CTA-861-H).
 *   Lower = dimmer desktop. Higher = brighter desktop.
 *
 * PEAK_NITS: display mastering peak — tells TV the max content brightness.
 *   Set to your display's actual peak (A85H OLED ≈ 800-1000 nits).
 *   This is now meaningful because we properly PQ-encode the signal.
 *
 * GAMUT: 0 = identity CTM, 100 = full BT.709→BT.2020 expansion.
 */
#define DEFAULT_SDR_NITS   203
#define DEFAULT_PEAK_NITS  800
#define DEFAULT_GAMUT      100
#define DEFAULT_GAMUT_MODE 0   /* 0=BT.2020, 1=DCI-P3 */
#define DEFAULT_MAX_BPC    10

static void load_conf(int *sdr_nits, int *peak_nits, int *gamut_pct,
                      int *gamut_mode, int *max_bpc) {
    *sdr_nits   = DEFAULT_SDR_NITS;
    *peak_nits  = DEFAULT_PEAK_NITS;
    *gamut_pct  = DEFAULT_GAMUT;
    *gamut_mode = DEFAULT_GAMUT_MODE;
    *max_bpc    = DEFAULT_MAX_BPC;
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int v; char s[64];
        if (sscanf(line, "SDR_NITS=%d",     &v) == 1) *sdr_nits   = v;
        if (sscanf(line, "PEAK_NITS=%d",    &v) == 1) *peak_nits  = v;
        if (sscanf(line, "GAMUT=%d",        &v) == 1) *gamut_pct  = v;
        if (sscanf(line, "MAX_BPC=%d",      &v) == 1) *max_bpc    = v;
        if (sscanf(line, "GAMUT_MODE=%63s", s)  == 1)
            *gamut_mode = (strncmp(s, "dci-p3", 6) == 0) ? 1 : 0;
    }
    fclose(f);
}

/* ── LUT helpers ─────────────────────────────────────────────────────────── */
typedef struct { uint16_t r, g, b, pad; } drm_lut_entry;

static double srgb_to_linear(double x) {
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}

/* SMPTE ST2084 (PQ) encode: linear light [0,1] → PQ code [0,1]
 * where linear 1.0 corresponds to 10000 cd/m². */
static double linear_to_pq(double L) {
    if (L <= 0.0) return 0.0;
    const double m1 = 0.1593017578125;
    const double m2 = 78.84375;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;
    double Lm = pow(L, m1);
    return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);
}

static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/* DEGAMMA: sRGB gamma → linear [0,1] */
static drm_lut_entry *build_degamma_srgb(int n) {
    drm_lut_entry *lut = calloc(n, sizeof(*lut));
    for (int i = 0; i < n; i++) {
        double x = (double)i / (n - 1);
        uint16_t v = (uint16_t)(clamp01(srgb_to_linear(x)) * 65535.0 + 0.5);
        lut[i].r = lut[i].g = lut[i].b = v;
    }
    return lut;
}

/* GAMMA: linear [0,1] → PQ code, where linear 1.0 = sdr_nits cd/m².
 * Matches KDE's approach: SDR white maps to sdr_nits on the PQ curve. */
static drm_lut_entry *build_gamma_pq(int n, double sdr_nits) {
    drm_lut_entry *lut = calloc(n, sizeof(*lut));
    double scale = sdr_nits / 10000.0;  /* normalize to PQ range */
    for (int i = 0; i < n; i++) {
        double x = (double)i / (n - 1); /* linear light [0,1], 1=SDR white */
        double pq = linear_to_pq(x * scale);
        uint16_t v = (uint16_t)(clamp01(pq) * 65535.0 + 0.5);
        lut[i].r = lut[i].g = lut[i].b = v;
    }
    return lut;
}

/* Identity LUT for reset */
static drm_lut_entry *build_linear_lut(int n) {
    drm_lut_entry *lut = calloc(n, sizeof(*lut));
    for (int i = 0; i < n; i++) {
        uint16_t v = (uint16_t)((double)i / (n - 1) * 65535.0 + 0.5);
        lut[i].r = lut[i].g = lut[i].b = v;
    }
    return lut;
}

/* ── CTM ─────────────────────────────────────────────────────────────────── */
/*
 * BT.709 → BT.2020 gamut expansion.
 * Derived from (BT.2020←XYZ) × (XYZ←BT.709), D65 white point.
 * Applied in linear light domain (between DEGAMMA and GAMMA).
 */
static const double CTM_709_TO_2020[3][3] = {
    { 0.627504,  0.329275,  0.043303 },
    { 0.069108,  0.919519,  0.011360 },
    { 0.016394,  0.088011,  0.895380 },
};

/*
 * BT.709 → DCI-P3 D65 gamut expansion.
 * Derived from (P3-D65←XYZ) × (XYZ←BT.709), D65 white point.
 * P3 is a middle ground: wider than sRGB, not as wide as BT.2020.
 */
static const double CTM_709_TO_DCIP3[3][3] = {
    { 0.822461,  0.177538,  0.000000 },
    { 0.033195,  0.966805,  0.000000 },
    { 0.017083,  0.072397,  0.910520 },
};

static void build_ctm(const double m[3][3], uint64_t out[9]) {
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
        double v = m[r][c];
        uint64_t mag = (uint64_t)(fabs(v) * (1ULL << 32)) & ~(1ULL << 63);
        if (v < 0) mag |= (1ULL << 63);
        out[r * 3 + c] = mag;
    }
}

static void build_ctm_identity(uint64_t out[9]) {
    double id[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    build_ctm(id, out);
}

/* ── HDR10 metadata ──────────────────────────────────────────────────────── */
/* Mirrors struct hdr_output_metadata from <drm/drm_mode.h>. sizeof == 32. */
typedef struct {
    uint32_t metadata_type;
    uint8_t  eotf;
    uint8_t  metadata_descriptor;
    uint16_t display_primaries[3][2];
    uint16_t white_point[2];
    uint16_t max_display_mastering_luminance;
    uint16_t min_display_mastering_luminance;
    uint16_t max_content_light_level;
    uint16_t max_frame_average_light_level;
} hdr_meta_t;

static hdr_meta_t build_hdr_meta(int peak_nits, int sdr_nits) {
    hdr_meta_t m = {0};
    m.metadata_type      = 0;   /* HDMI_STATIC_METADATA_TYPE1 */
    m.eotf               = 2;   /* PQ / ST2084 */
    m.metadata_descriptor = 0;
    /* BT.2020 primaries × 50000 (CTA-861 order: G, B, R) */
    m.display_primaries[0][0] = (uint16_t)(0.170 * 50000); /* G x */
    m.display_primaries[0][1] = (uint16_t)(0.797 * 50000); /* G y */
    m.display_primaries[1][0] = (uint16_t)(0.131 * 50000); /* B x */
    m.display_primaries[1][1] = (uint16_t)(0.046 * 50000); /* B y */
    m.display_primaries[2][0] = (uint16_t)(0.708 * 50000); /* R x */
    m.display_primaries[2][1] = (uint16_t)(0.292 * 50000); /* R y */
    m.white_point[0] = (uint16_t)(0.3127 * 50000);         /* D65 */
    m.white_point[1] = (uint16_t)(0.3290 * 50000);
    m.max_display_mastering_luminance = (uint16_t)peak_nits;
    m.min_display_mastering_luminance = 1;                  /* 0.0001 cd/m² OLED */
    m.max_content_light_level        = (uint16_t)peak_nits;
    m.max_frame_average_light_level  = (uint16_t)sdr_nits;
    return m;
}

/* ── property helpers ────────────────────────────────────────────────────── */
static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return 0;
    uint32_t result = 0;
    for (uint32_t i = 0; i < props->count_props && !result; i++) {
        drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
        if (p) {
            if (strcmp(p->name, name) == 0) result = p->prop_id;
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(props);
    return result;
}

static uint64_t get_enum_val(int fd, uint32_t prop_id, const char *enum_name) {
    drmModePropertyPtr p = drmModeGetProperty(fd, prop_id);
    if (!p) return 0;
    uint64_t val = 0; int found = 0;
    for (int i = 0; i < p->count_enums; i++) {
        if (strcmp(p->enums[i].name, enum_name) == 0) {
            val = (uint64_t)p->enums[i].value; found = 1; break;
        }
    }
    if (!found) {
        printf("  enum '%s' not found on prop %u; available:", enum_name, prop_id);
        for (int i = 0; i < p->count_enums; i++)
            printf(" %s=%llu", p->enums[i].name, (unsigned long long)p->enums[i].value);
        printf("\n");
    }
    drmModeFreeProperty(p);
    return found ? val : 0;
}

static uint32_t mk_blob(int fd, const void *data, size_t sz) {
    uint32_t id = 0;
    drmModeCreatePropertyBlob(fd, data, sz, &id);
    return id;
}

/* ── VT switch ───────────────────────────────────────────────────────────── */
static int vt_switch(int tty_fd, int target_vt) {
    if (ioctl(tty_fd, VT_ACTIVATE,  target_vt) < 0) { perror("VT_ACTIVATE");  return -1; }
    if (ioctl(tty_fd, VT_WAITACTIVE, target_vt) < 0) { perror("VT_WAITACTIVE"); return -1; }
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    int reset = 0, sdr_nits, peak_nits, gamut_pct, gamut_mode, max_bpc;
    load_conf(&sdr_nits, &peak_nits, &gamut_pct, &gamut_mode, &max_bpc);

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "reset") == 0)              reset      = 1;
        else if (strcmp(argv[i], "--sdr-nits")  == 0 && i+1 < argc) sdr_nits   = atoi(argv[++i]);
        else if (strcmp(argv[i], "--peak-nits") == 0 && i+1 < argc) peak_nits  = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gamut")     == 0 && i+1 < argc) gamut_pct  = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bpc")       == 0 && i+1 < argc) max_bpc    = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gamut-mode") == 0 && i+1 < argc) {
            gamut_mode = (strcmp(argv[++i], "dci-p3") == 0) ? 1 : 0;
        }
    }
    const char *gmode_str = (gamut_mode == 1) ? "DCI-P3" : "BT.2020";
    printf("config: sdr_nits=%d  peak_nits=%d  gamut=%d%%  mode=%s  bpc=%d\n",
           sdr_nits, peak_nits, gamut_pct, gmode_str, max_bpc);

    if (geteuid() != 0) { fprintf(stderr, "run as root: sudo cosmic-hdr\n"); return 1; }

    int tty_fd = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (tty_fd < 0) { perror("open /dev/tty1"); return 1; }

    printf("switching to VT2 (screen will blank ~0.5s)...\n");
    if (vt_switch(tty_fd, 2) < 0) {
        fprintf(stderr, "VT switch failed\n"); close(tty_fd); return 1;
    }
    usleep(400000);

    int fd = open(DRM_DEV, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open " DRM_DEV); vt_switch(tty_fd, 1); return 1; }

    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        fprintf(stderr, "drmSetClientCap: %s\n", strerror(errno));
        vt_switch(tty_fd, 1); return 1;
    }
    if (drmSetMaster(fd) != 0) {
        fprintf(stderr, "drmSetMaster: %s\n", strerror(errno));
        close(fd); vt_switch(tty_fd, 1); return 1;
    }
    printf("DRM master acquired\n");

    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) { perror("drmModeGetResources"); drmDropMaster(fd); vt_switch(tty_fd, 1); return 1; }

    uint32_t conn_id = 0, crtc_id = 0;
    for (int i = 0; i < res->count_connectors && !conn_id; i++) {
        drmModeConnectorPtr c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connector_type == DRM_MODE_CONNECTOR_HDMIA && c->connection == DRM_MODE_CONNECTED) {
            conn_id = c->connector_id;
            if (c->encoder_id) {
                drmModeEncoderPtr enc = drmModeGetEncoder(fd, c->encoder_id);
                if (enc) { crtc_id = enc->crtc_id; drmModeFreeEncoder(enc); }
            }
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);

    if (!conn_id || !crtc_id) {
        fprintf(stderr, "HDMI-A connector/CRTC not found\n");
        drmDropMaster(fd); vt_switch(tty_fd, 1); return 1;
    }
    printf("connector=%u  CRTC=%u\n", conn_id, crtc_id);

    /* ── Build LUTs and CTM ─────────────────────────────────────────────── */
    drm_lut_entry *deg_lut, *gam_lut;
    uint64_t ctm9[9];

    if (reset) {
        deg_lut = build_linear_lut(LUT_SIZE);
        gam_lut = build_linear_lut(LUT_SIZE);
        build_ctm_identity(ctm9);
    } else {
        deg_lut = build_degamma_srgb(LUT_SIZE);
        gam_lut = build_gamma_pq(LUT_SIZE, (double)sdr_nits);
        double t = gamut_pct / 100.0;
        double blended[3][3];
        const double (*target)[3] = (gamut_mode == 1) ? CTM_709_TO_DCIP3 : CTM_709_TO_2020;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                blended[r][c] = (r==c ? 1.0 : 0.0) * (1.0-t) + target[r][c] * t;
        build_ctm((const double(*)[3])blended, ctm9);
    }

    uint32_t deg_blob = mk_blob(fd, deg_lut, LUT_SIZE * sizeof(drm_lut_entry));
    uint32_t gam_blob = mk_blob(fd, gam_lut, LUT_SIZE * sizeof(drm_lut_entry));
    uint32_t ctm_blob = mk_blob(fd, ctm9, sizeof(ctm9));
    free(deg_lut); free(gam_lut);
    printf("blobs: DEGAMMA=%u CTM=%u GAMMA=%u\n", deg_blob, ctm_blob, gam_blob);

    /* ── Get property IDs ───────────────────────────────────────────────── */
    uint32_t p_deg    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,     "DEGAMMA_LUT");
    uint32_t p_ctm    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,     "CTM");
    uint32_t p_gam    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,     "GAMMA_LUT");
    uint32_t p_hdr    = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "HDR_OUTPUT_METADATA");
    uint32_t p_cspace = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "Colorspace");
    printf("props: DEGAMMA=%u CTM=%u GAMMA=%u HDR=%u Colorspace=%u\n",
           p_deg, p_ctm, p_gam, p_hdr, p_cspace);

    /* ── Commit color pipeline ──────────────────────────────────────────── */
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    if (p_deg) drmModeAtomicAddProperty(req, crtc_id, p_deg, reset ? 0 : deg_blob);
    if (p_ctm) drmModeAtomicAddProperty(req, crtc_id, p_ctm, reset ? 0 : ctm_blob);
    if (p_gam) drmModeAtomicAddProperty(req, crtc_id, p_gam, reset ? 0 : gam_blob);

    int ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
    printf("color pipeline commit ret=%d  errno=%d (%s)\n", ret, errno, ret ? strerror(errno) : "ok");
    drmModeAtomicFree(req);

    /* ── Commit HDR metadata + colorspace (needs ALLOW_MODESET) ────────── */
    int hdr_ret = -1;
    if (p_hdr && p_cspace) {
        uint64_t cspace_val = reset ? 0 : get_enum_val(fd, p_cspace, "BT2020_RGB");

        hdr_meta_t hdr_m    = build_hdr_meta(peak_nits, sdr_nits);
        uint32_t hdr_blob   = (reset || cspace_val == 0) ? 0
                              : mk_blob(fd, &hdr_m, sizeof(hdr_m));

        uint32_t p_crtc_id  = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        uint32_t p_crtc_act = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,      "ACTIVE");
        uint32_t p_mode_id  = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,      "MODE_ID");
        uint32_t p_bpc      = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "max_requested_bpc");

        drmModeCrtcPtr cur = drmModeGetCrtc(fd, crtc_id);
        uint32_t mode_blob = 0;
        if (cur && p_mode_id) {
            drmModeCreatePropertyBlob(fd, &cur->mode, sizeof(cur->mode), &mode_blob);
            drmModeFreeCrtc(cur);
        }

        drmModeAtomicReqPtr req2 = drmModeAtomicAlloc();
        if (p_crtc_id)  drmModeAtomicAddProperty(req2, conn_id, p_crtc_id,  crtc_id);
        drmModeAtomicAddProperty(req2, conn_id, p_hdr,    hdr_blob);
        drmModeAtomicAddProperty(req2, conn_id, p_cspace, cspace_val);
        if (p_bpc && !reset) drmModeAtomicAddProperty(req2, conn_id, p_bpc, (uint64_t)max_bpc);
        if (p_crtc_act) drmModeAtomicAddProperty(req2, crtc_id, p_crtc_act, 1);
        if (p_mode_id && mode_blob)
                        drmModeAtomicAddProperty(req2, crtc_id, p_mode_id,   mode_blob);

        hdr_ret = drmModeAtomicCommit(fd, req2, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        printf("HDR+Colorspace commit ret=%d  errno=%d (%s)\n",
               hdr_ret, errno, hdr_ret ? strerror(errno) : "ok");
        drmModeAtomicFree(req2);
        if (hdr_blob)  drmModeDestroyPropertyBlob(fd, hdr_blob);
        if (mode_blob) drmModeDestroyPropertyBlob(fd, mode_blob);
    }

    drmDropMaster(fd);
    drmModeDestroyPropertyBlob(fd, deg_blob);
    drmModeDestroyPropertyBlob(fd, ctm_blob);
    drmModeDestroyPropertyBlob(fd, gam_blob);
    close(fd);

    printf("DRM master released — switching back to VT1...\n");
    vt_switch(tty_fd, 1);
    close(tty_fd);

    if (ret == 0 && hdr_ret == 0) {
        if (reset)
            printf("✓ reset: SDR restored\n");
        else
            printf("✓ HDR10 ACTIVE: sRGB→linear→%s→PQ pipeline live\n"
                   "  SDR white=%d nits  peak=%d nits  gamut=%d%%  bpc=%d\n",
                   gmode_str, sdr_nits, peak_nits, gamut_pct, max_bpc);
    } else {
        printf("pipeline ret=%d  HDR ret=%d\n", ret, hdr_ret);
    }
    return (ret != 0);
}
