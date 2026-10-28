#pragma once

/*  Every per-star value ASTRA stores, described well enough to read and write
 *  it generically: key (the database column), display name, category, unit,
 *  type, whether it may be edited, a getter and a setter.
 *
 *  Built on Star::getFieldMap() (the getters the star table uses) and the
 *  column registry (display names and categories), so it cannot drift from
 *  either. The AI tools and the star-field editor go through it.
 *
 *  Fields that ASTRA derives from fits and child data (teff/logg/he from the
 *  best spectral fit, rv_* from the RV curve, sed_* from the best SED fit,
 *  has_* and n_spectra from what is loaded) are not editable: the next
 *  summary recomputation would silently overwrite them. Change the fit
 *  instead.                                                                  */

#include <QJsonValue>
#include <QString>
#include <QVariant>

#include <functional>
#include <vector>

class Star;

struct StarField {
    enum class Type { Double, Int, Bool, String };

    QString key;
    QString displayName;
    QString category;
    QString unit;          ///< "" when dimensionless or not applicable
    QString note;          ///< why a field is read-only, or what it means
    Type    type = Type::Double;
    bool    editable = false;

    std::function<QVariant(const Star*)>           get;
    std::function<void(Star&, const QVariant&)>    set;   ///< empty when not editable

    static QString typeName(Type t);
};

class StarFieldRegistry {
public:
    static const StarFieldRegistry& instance();

    const std::vector<StarField>& all() const { return _fields; }
    const StarField* find(const QString& key) const;

    /// The value as JSON: numbers, strings, booleans; null for unset (NaN or "").
    QJsonValue valueJson(const Star& star, const StarField& f) const;
    QJsonValue valueJson(const Star& star, const QString& key) const;

    /// Sets one field from JSON. null clears a numeric field (NaN). Returns
    /// false with a reason when the field is unknown, read-only or the value
    /// has the wrong type.
    bool setJson(Star& star, const QString& key, const QJsonValue& value, QString* error) const;

private:
    StarFieldRegistry();
    std::vector<StarField> _fields;
};
