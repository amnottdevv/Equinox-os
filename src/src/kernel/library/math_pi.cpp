#include "header/math_pi.h"
#include "header/math2.h"

// ============================================================
//  0.4 Beta: sinf/cosf/tanf kini diimplementasikan x87 hardware di
//  math2.cpp (akurat penuh, bukan Taylor series lama yang hanya
//  presisi dekat 0). File ini tinggal konversi sudut — definisi
//  lama DIHAPUS agar tidak bentrok simbol di link (multiple
//  definition sinf/cosf/tanf).
// ============================================================

float deg_to_rad(float deg) {
    return deg * (PI / 180.0f);
}

float rad_to_deg(float rad) {
    return rad * (180.0f / PI);
}
