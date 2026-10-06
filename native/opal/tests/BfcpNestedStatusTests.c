#include "bfcp_messages.h"

static int expect(unsigned char *bytes, unsigned length, int accepted)
{
    bfcp_message message = { bytes, 0, (UINT16)length };
    bfcp_received_attribute attribute = { FLOOR_REQUEST_INFORMATION, 1, (int)length, 0, 1, NULL };
    bfcp_floor_request_information *parsed = bfcp_parse_attribute_FLOOR_REQUEST_INFORMATION(&message, &attribute);
    int ok = accepted ? parsed && parsed->frqID == 7 && parsed->fRS && parsed->fRS->fID == 2 : parsed == NULL;
    if (ok && accepted == 1) ok = parsed->fRS->rs && parsed->fRS->rs->rs == BFCP_GRANTED;
    if (ok && accepted == 2) ok = !parsed->fRS->rs && parsed->oRS && parsed->oRS->rs && parsed->oRS->rs->rs == BFCP_GRANTED;
    if (ok && accepted == 3) ok = !parsed->fRS->rs && parsed->oRS && !parsed->oRS->rs;
    if (parsed) bfcp_free_floor_request_information_list(parsed);
    return ok;
}

extern int mystudio_bfcp_fsm_safety_tests(void);
static int optional_status(void)
{
    unsigned char valid[] = { (FLOOR_REQUEST_INFORMATION << 1) | 1, 16, 0, 7,
        (OVERALL_REQUEST_STATUS << 1) | 1, 8, 0, 7,
        (REQUEST_STATUS << 1) | 1, 4, BFCP_GRANTED, 0,
        (FLOOR_REQUEST_STATUS << 1) | 1, 4, 0, 2 };
    unsigned char absent[] = { (FLOOR_REQUEST_INFORMATION << 1) | 1, 12, 0, 7,
        (OVERALL_REQUEST_STATUS << 1) | 1, 4, 0, 7,
        (FLOOR_REQUEST_STATUS << 1) | 1, 4, 0, 2 };
    unsigned char bad[sizeof(valid)];
    if (!expect(absent, sizeof(absent), 3)) return 11; /* No received status means no grant. */
    if (!expect(valid, sizeof(valid), 2)) return 12; /* Received overall grant and actual floor2. */
    memcpy(bad,valid,sizeof(bad));bad[5]=12;
    if (!expect(bad,sizeof(bad),0)) return 13; /* Overall child crosses into floor sibling. */
    memcpy(bad,valid,sizeof(bad));bad[9]=0;
    if (!expect(bad,sizeof(bad),0)) return 14;
    memcpy(bad,valid,sizeof(bad));bad[10]=255;
    if (!expect(bad,sizeof(bad),0)) return 15;
    return 0;
}
int main(int argc, char **argv)
{
    int result;
    if (argc > 1 && strcmp(argv[1], "--fsm-only") == 0) return mystudio_bfcp_fsm_safety_tests();
    if (argc > 1 && strcmp(argv[1], "--overall-only") == 0) return optional_status();
    /* Real grouped FloorRequestStatus: request7 / floor2 / Granted. */
    unsigned char valid[] = { (FLOOR_REQUEST_INFORMATION << 1) | 1, 12, 0, 7,
        (FLOOR_REQUEST_STATUS << 1) | 1, 8, 0, 2,
        (REQUEST_STATUS << 1) | 1, 4, BFCP_GRANTED, 0 };
    unsigned char bad[sizeof(valid)];
    if (!expect(valid, sizeof(valid), 1)) { fputs("Valid nested Granted floor was lost\n", stderr); return 1; }
    memcpy(bad, valid, sizeof(bad)); bad[5] = 2;
    if (!expect(bad, sizeof(bad), 0)) return 2; /* Truncated floor header. */
    memcpy(bad, valid, sizeof(bad)); bad[5] = 16;
    if (!expect(bad, sizeof(bad), 0)) return 3; /* Child escapes parent/message. */
    memcpy(bad, valid, sizeof(bad)); bad[9] = 0;
    if (!expect(bad, sizeof(bad), 0)) return 5; /* Zero child length. */
    memcpy(bad, valid, sizeof(bad)); bad[10] = 255;
    if (!expect(bad, sizeof(bad), 0)) return 6; /* Unknown request status. */
    memcpy(bad, valid, sizeof(bad)); bad[7] = 0;
    if (!expect(bad, sizeof(bad), 0)) return 7; /* No assumed floor ID. */
    if (!expect(valid, 4, 0)) return 8; /* Missing mandatory nested floor. */
    if (!expect(valid, 11, 0)) return 9; /* Last status byte is truncated. */
    {
        unsigned char duplicate[16];
        memcpy(duplicate, valid, sizeof(valid));
        memcpy(duplicate+12, valid+8, 4);
        duplicate[1] = 16; duplicate[5] = 12;
        if (!expect(duplicate, sizeof(duplicate), 0)) return 10;
    }
    result=optional_status();if(result)return result;
    result=mystudio_bfcp_fsm_safety_tests();if(result)return result;
    puts("Actual nested/optional BFCP Granted retained; malformed/missing/cross-boundary status rejected PASS");return 0;
}
