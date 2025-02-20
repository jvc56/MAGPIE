typedef struct BAITracking BAITracking;

typedef enum { BAI_CTRACKING, BAI_DTRACKING } bai_tracking_t;

BAITracking *bai_tracking_create(bai_tracking_t type, int *N, int size);
void bai_tracking_destroy(BAITracking *bai_tracking);
int bai_track(BAITracking *bai_tracking, int *N, double *w, int size);