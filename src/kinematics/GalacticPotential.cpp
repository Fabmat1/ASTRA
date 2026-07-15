#include "GalacticPotential.h"

#include <cmath>

namespace GalKin {

GalacticPotential::GalacticPotential(Model model) : _model(model)
{
    switch (model) {
    case Model::AS: // Irrgang et al. 2013, Model I
        _Mb = 409.0;  _Md = 2856.0; _Mh = 1018.0;
        _bb = 0.23;   _ad = 4.22;   _bd = 0.292;  _ah = 2.562;
        _gamma = 2.0; _cutoff = 200.0;
        break;
    case Model::MN_TF: // Model II
        _Mb = 175.0;  _Md = 2829.0; _Mh = 69725.0;
        _bb = 0.184;  _ad = 4.85;   _bd = 0.305;  _ah = 200.0;
        _gamma = 0.0; _cutoff = 0.0;
        break;
    case Model::MN_NFW: // Model III
        _Mb = 439.0;  _Md = 3096.0; _Mh = 142200.0;
        _bb = 0.236;  _ad = 3.262;  _bd = 0.289;  _ah = 45.02;
        _gamma = 0.0; _cutoff = 0.0;
        break;
    }
}

double GalacticPotential::sunGCDist() const
{
    switch (_model) {
    case Model::AS:     return 8.40;
    case Model::MN_TF:  return 8.35;
    case Model::MN_NFW: return 8.33;
    }
    return 8.40;
}

const char* GalacticPotential::modelName(Model m)
{
    switch (m) {
    case Model::AS:     return "Model I (Allen & Santillan revised)";
    case Model::MN_TF:  return "Model II (MN disc + truncated flat halo)";
    case Model::MN_NFW: return "Model III (MN disc + NFW halo)";
    }
    return "";
}

// (dΦ_halo/dR)/R in model units; R² = r²+z². Positive = inward pull.
double GalacticPotential::haloGradOverR(double R2) const
{
    constexpr double eps = 1e-5;
    const double R = std::sqrt(R2);
    if (R < eps)
        return 0.0; // regularized at the origin

    switch (_model) {
    case Model::AS: {
        const double ca = _cutoff / _ah; // cutoff/ah
        if (R < _cutoff) {
            const double p = std::pow(R / _ah, _gamma - 1.0);
            return _Mh * p / R2 / (1.0 + p) / _ah;
        }
        // beyond the cutoff the halo acts as a point mass
        return _Mh * std::pow(ca, _gamma) /
               (R2 * R) / (1.0 + std::pow(ca, _gamma - 1.0));
    }
    case Model::MN_TF:
        return _Mh / std::sqrt(R2 + _ah * _ah) / R2;
    case Model::MN_NFW:
        return -(_Mh / R2 / _ah / (1.0 + R / _ah) -
                 _Mh * std::log(1.0 + R / _ah) / (R2 * R));
    }
    return 0.0;
}

double GalacticPotential::haloPotential(double R) const
{
    constexpr double eps = 1e-5;
    if (R < eps)
        R = eps; // regularized at the origin

    switch (_model) {
    case Model::AS: {
        const double ca  = std::pow(_cutoff / _ah, _gamma - 1.0);
        if (R < _cutoff) {
            const double p = std::pow(R / _ah, _gamma - 1.0);
            return _Mh * std::log((1.0 + p) / (1.0 + ca)) / (_gamma - 1.0) / _ah
                 - _Mh * ca / (1.0 + ca) / _ah;
        }
        return -_Mh * ca * (_cutoff / _ah) / R / (1.0 + ca);
    }
    case Model::MN_TF:
        return -_Mh / _ah *
               std::log((std::sqrt(R * R + _ah * _ah) + _ah) / R);
    case Model::MN_NFW:
        return -_Mh * std::log(1.0 + R / _ah) / R;
    }
    return 0.0;
}

Vec3 GalacticPotential::acceleration(const Vec3& pos) const
{
    const double x = pos[0], y = pos[1], z = pos[2];
    const double r2   = x * x + y * y;
    const double z2   = z * z;
    const double R2   = r2 + z2;
    const double szbd = std::sqrt(z2 + _bd * _bd);
    const double adz  = _ad + szbd;

    // (dΦ/dR)/R for bulge and (dΦ/d·)/· for the disc's planar part
    const double gBulge = _Mb / std::pow(R2 + _bb * _bb, 1.5);
    const double gDisc  = _Md / std::pow(r2 + adz * adz, 1.5);
    const double gHalo  = haloGradOverR(R2);

    const double gxy = gBulge + gDisc + gHalo;
    const double gz  = gBulge + gDisc * adz / szbd + gHalo;

    return { -kAccelConv * gxy * x,
             -kAccelConv * gxy * y,
             -kAccelConv * gz * z };
}

double GalacticPotential::potentialKm2S2(const Vec3& pos) const
{
    const double x = pos[0], y = pos[1], z = pos[2];
    const double r2   = x * x + y * y;
    const double z2   = z * z;
    const double R2   = r2 + z2;
    const double szbd = std::sqrt(z2 + _bd * _bd);
    const double adz  = _ad + szbd;

    const double phi = -_Mb / std::sqrt(R2 + _bb * _bb)
                       - _Md / std::sqrt(r2 + adz * adz)
                       + haloPotential(std::sqrt(R2));
    return 100.0 * phi; // model units are 100 km²/s²
}

double GalacticPotential::totalEnergyKm2S2(const Vec3& pos,
                                           const Vec3& velKmS) const
{
    const double v2 = velKmS[0] * velKmS[0] + velKmS[1] * velKmS[1] +
                      velKmS[2] * velKmS[2];
    return 0.5 * v2 + potentialKm2S2(pos);
}

double GalacticPotential::circularVelocityKmS(double r) const
{
    const Vec3 a = acceleration({r, 0.0, 0.0});
    // v_circ² = r · |dΦ/dr| ; a[0] is in kpc/Myr², convert to km²/s²
    const double v2 = -r * a[0] * kKpcPerMyrInKmS * kKpcPerMyrInKmS;
    return v2 > 0.0 ? std::sqrt(v2) : 0.0;
}

double GalacticPotential::escapeVelocityKmS(const Vec3& pos) const
{
    const double phi = potentialKm2S2(pos);
    return phi < 0.0 ? std::sqrt(-2.0 * phi) : 0.0;
}

} // namespace GalKin
