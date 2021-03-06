/*
 * Copyright (C) 2013 Jolla Mobile <andrew.den.exter@jollamobile.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * "Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Nemo Mobile nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
 */

#include "seasidefilteredmodel.h"
#include "seasideperson.h"

#include <synchronizelists.h>

#include <qtcontacts-extensions.h>
#include <qtcontacts-extensions_impl.h>

#include <QContactStatusFlags>

#include <QContactAvatar>
#include <QContactEmailAddress>
#include <QContactFavorite>
#include <QContactName>
#include <QContactNickname>
#include <QContactOnlineAccount>
#include <QContactOrganization>
#include <QContactPhoneNumber>
#include <QContactGlobalPresence>
#include <QContactPresence>
#include <QContactNote>
#include <QTextBoundaryFinder>

#include <MLocale>
#include <MBreakIterator>

#include <QCoreApplication>
#include <QEvent>
#include <QVector>
#include <QtDebug>

namespace {

const QByteArray displayLabelRole("displayLabel");
const QByteArray firstNameRole("firstName");        // To be deprecated
const QByteArray lastNameRole("lastName");          // To be deprecated
const QByteArray sectionBucketRole("sectionBucket");
const QByteArray favoriteRole("favorite");
const QByteArray avatarRole("avatar");
const QByteArray avatarUrlRole("avatarUrl");
const QByteArray globalPresenceStateRole("globalPresenceState");
const QByteArray contactIdRole("contactId");
const QByteArray phoneNumbersRole("phoneNumbers");
const QByteArray emailAddressesRole("emailAddresses");
const QByteArray accountUrisRole("accountUris");
const QByteArray accountPathsRole("accountPaths");
const QByteArray personRole("person");
const QByteArray primaryNameRole("primaryName");
const QByteArray secondaryNameRole("secondaryName");
const QByteArray nicknameDetailsRole("nicknameDetails");
const QByteArray phoneDetailsRole("phoneDetails");
const QByteArray emailDetailsRole("emailDetails");
const QByteArray accountDetailsRole("accountDetails");
const QByteArray noteDetailsRole("noteDetails");
const QByteArray companyNameRole("companyName");
const QByteArray titleRole("title");
const QByteArray roleRole("role");

const ML10N::MLocale mLocale;

template<typename T>
void insert(QList<T> &dst, const QList<T> &src)
{
    for (const T &item : src)
        dst.append(item);
}

QSet<QString> alphabetCharacters()
{
    QSet<QString> rv;

    for (const QString &c : mLocale.exemplarCharactersIndex()) {
        rv.insert(mLocale.toLower(c));
    }

    return rv;
}

QMap<uint, QString> decompositionMapping()
{
    QMap<uint, QString> rv;

    rv.insert(0x00df, QStringLiteral("ss")); // sharp-s ('sz' ligature)
    rv.insert(0x00e6, QStringLiteral("ae")); // 'ae' ligature
    rv.insert(0x00f0, QStringLiteral("d"));  // eth
    rv.insert(0x00f8, QStringLiteral("o"));  // o with stroke
    rv.insert(0x00fe, QStringLiteral("th")); // thorn
    rv.insert(0x0111, QStringLiteral("d"));  // d with stroke
    rv.insert(0x0127, QStringLiteral("h"));  // h with stroke
    rv.insert(0x0138, QStringLiteral("k"));  // kra
    rv.insert(0x0142, QStringLiteral("l"));  // l with stroke
    rv.insert(0x014b, QStringLiteral("n"));  // eng
    rv.insert(0x0153, QStringLiteral("oe")); // 'oe' ligature
    rv.insert(0x0167, QStringLiteral("t"));  // t with stroke
    rv.insert(0x017f, QStringLiteral("s"));  // long s

    return rv;
}

QStringList tokenize(const QString &word)
{
    static const QSet<QString> alphabet(alphabetCharacters());
    static const QMap<uint, QString> decompositions(decompositionMapping());

    // Convert the word to canonical form, lowercase
    QString canonical(word.normalized(QString::NormalizationForm_C));

    QStringList tokens;

    ML10N::MBreakIterator it(mLocale, canonical, ML10N::MBreakIterator::CharacterIterator);
    while (it.hasNext()) {
        const int position = it.next();
        const int nextPosition = it.peekNext();
        if (position < nextPosition) {
            const QString character(canonical.mid(position, (nextPosition - position)));
            QStringList matches;
            if (alphabet.contains(character)) {
                // This character is a member of the alphabet for this locale - do not decompose it
                matches.append(character);
            } else {
                // This character is not a member of the alphabet; decompose it to
                // assist with diacritic-insensitive matching
                QString normalized(character.normalized(QString::NormalizationForm_D));
                matches.append(normalized);

                // For some characters, we want to match alternative spellings that do not correspond
                // to decomposition characters
                const uint codePoint(normalized.at(0).unicode());
                QMap<uint, QString>::const_iterator dit = decompositions.find(codePoint);
                if (dit != decompositions.end()) {
                    matches.append(*dit);
                }
            }

            if (tokens.isEmpty()) {
                tokens.append(QString());
            }

            int previousCount = tokens.count();
            for (int i = 1; i < matches.count(); ++i) {
                // Make an additional copy of the existing tokens, for each new possible match
                for (int j = 0; j < previousCount; ++j) {
                    tokens.append(tokens.at(j) + matches.at(i));
                }
            }
            for (int j = 0; j < previousCount; ++j) {
                tokens[j].append(matches.at(0));
            }
        }
    }

    return tokens;
}

QList<const QString *> makeSearchToken(const QString &word)
{
    static QMap<uint, const QString *> indexedTokens;
    static QHash<QString, QList<const QString *> > indexedWords;

    // Index all search text in lower case
    const QString lowered(mLocale.toLower(word));

    QHash<QString, QList<const QString *> >::const_iterator wit = indexedWords.find(lowered);
    if (wit == indexedWords.end()) {
        QList<const QString *> indexed;

        // Index these tokens for later dereferencing
        for (const QString &token : tokenize(lowered)) {
            uint hashValue(qHash(token));
            QMap<uint, const QString *>::const_iterator tit = indexedTokens.find(hashValue);
            if (tit == indexedTokens.end()) {
                tit = indexedTokens.insert(hashValue, new QString(token));
            }
            indexed.append(*tit);
        }

        wit = indexedWords.insert(lowered, indexed);
    }

    return *wit;
}

// Splits a string at word boundaries identified by MBreakIterator
QList<const QString *> splitWords(const QString &string)
{
    QList<const QString *> rv;
    if (!string.isEmpty()) {
        // Ignore any instances of '.' (frequently present in email addresses, but not useful)
        const QString dot(QStringLiteral("."));
        ML10N::MBreakIterator it(mLocale, string, ML10N::MBreakIterator::WordIterator);
        while (it.hasNext()) {
            const int position = it.next();
            const QString word(string.mid(position, (it.peekNext() - position)).trimmed());
            if (!word.isEmpty() && word != dot) {
                for (const QString *alternative : makeSearchToken(word)) {
                    rv.append(alternative);
                }
            }
        }
    }

    return rv;
}

QList<QStringList> extractSearchTerms(const QString &string)
{
    QList<QStringList> rv;

    // Test all searches in lower case
    const QString lowered(mLocale.toLower(string));

    ML10N::MBreakIterator it(mLocale, lowered, ML10N::MBreakIterator::WordIterator);
    while (it.hasNext()) {
        const int position = it.next();
        const QString word(lowered.mid(position, (it.peekNext() - position)).trimmed());
        if (!word.isEmpty()) {
            const bool apostrophe(word.length() == 1 && word.at(0) == QChar('\''));
            if (apostrophe && !rv.isEmpty()) {
                // Special case - a trailing apostrophe is not counted as a component of the
                // previous word, although it is included in the word if there is a following character
                rv.last().last().append(word);
            } else {
                rv.append(tokenize(word));
            }
        }
    }

    return rv;
}

QString stringPreceding(const QString &s, const QChar &c)
{
    int index = s.indexOf(c);
    if (index != -1) {
        return s.left(index);
    }
    return s;
}

struct LessThanIndirect {
    template<typename T>
    bool operator()(T lhs, T rhs) const { return *lhs < *rhs; }
};

struct EqualIndirect {
    template<typename T>
    bool operator()(T lhs, T rhs) const { return *lhs == *rhs; }
};

struct FirstElementLessThanIndirect {
    template<typename Container>
    bool operator()(const Container *lhs, typename Container::const_iterator rhs) { return *lhs->cbegin() < *rhs; }

    template<typename Container>
    bool operator()(typename Container::const_iterator lhs, const Container *rhs) { return *lhs < *rhs->cbegin(); }
};

template<template <typename T> class Container, typename T>
QVector<T> toSortedVector(Container<T> &src)
{
    QVector<T> dst;

    std::sort(src.begin(), src.end(), LessThanIndirect());
    src.erase(std::unique(src.begin(), src.end(), EqualIndirect()), src.end());

    dst.reserve(src.count());
    std::copy(src.cbegin(), src.cend(), std::back_inserter(dst));
    return dst;
}

}

struct FilterData : public SeasideCache::ItemListener
{
    // Store additional filter keys with the cache item
    QVector<const QString *> initialMatchKeys;
    QVector<const QString *> wildMatchKeys;

    static FilterData *getItemFilterData(SeasideCache::CacheItem *item, const SeasideFilteredModel *model)
    {
        void *key = const_cast<void *>(static_cast<const void *>(model));
        SeasideCache::ItemListener *listener = item->listener(key);
        if (!listener) {
            listener = item->appendListener(new FilterData, key);
        }

        return static_cast<FilterData *>(listener);
    }

    bool prepareFilter(SeasideCache::CacheItem *item)
    {
        static const QChar atSymbol(QChar::fromLatin1('@'));
        static const QtContactsSqliteExtensions::NormalizePhoneNumberFlags normalizeFlags(QtContactsSqliteExtensions::KeepPhoneNumberDialString |
                                                                                          QtContactsSqliteExtensions::ValidatePhoneNumber);

        if (wildMatchKeys.isEmpty()) {
            QList<const QString *> matchTokens;
            matchTokens.reserve(100);

            for (const QContactOnlineAccount &detail : item->contact.details<QContactOnlineAccount>())
                insert(matchTokens, splitWords(stringPreceding(detail.accountUri(), atSymbol)));
            for (const QContactGlobalPresence &detail : item->contact.details<QContactGlobalPresence>())
                insert(matchTokens, splitWords(detail.nickname()));
            for (const QContactPresence &detail : item->contact.details<QContactPresence>())
                insert(matchTokens, splitWords(detail.nickname()));

            initialMatchKeys = toSortedVector(matchTokens);

            // split the display label and filter details into words
            QContactName name = item->contact.detail<QContactName>();

            // First name, middle name, last name, prefix, suffix, custom field, nick name,
            // email address, organization, and phone number can be matched in the middle.
            matchTokens.clear();
            insert(matchTokens, splitWords(name.firstName()));
            insert(matchTokens, splitWords(name.middleName()));
            insert(matchTokens, splitWords(name.lastName()));
            insert(matchTokens, splitWords(name.prefix()));
            insert(matchTokens, splitWords(name.suffix()));

            // Include the custom label - it may contain the user's customized name for the contact
            insert(matchTokens, splitWords(name.value<QString>(QContactName__FieldCustomLabel)));

            if (matchTokens.isEmpty()) {
                // The contact must have some kind of identifying marks
                insert(matchTokens, splitWords(item->displayLabel));
            }

            for (const QContactNickname &detail : item->contact.details<QContactNickname>()) {
                insert(matchTokens, splitWords(detail.nickname()));
            }
            for (const QContactEmailAddress &detail : item->contact.details<QContactEmailAddress>()) {
                insert(matchTokens, splitWords(stringPreceding(detail.emailAddress(), atSymbol)));
            }
            for (const QContactOrganization &detail : item->contact.details<QContactOrganization>()) {
                insert(matchTokens, splitWords(detail.name()));
            }

            // Add phone numbers to a separate list where we will match any part of the string
            QList<QContactPhoneNumber> phoneNumbers(item->contact.details<QContactPhoneNumber>());
            if (!phoneNumbers.isEmpty()) {
                for (const QContactPhoneNumber &detail : phoneNumbers) {
                    // For phone numbers, match on the normalized from (punctuation stripped)
                    QString normalized(QtContactsSqliteExtensions::normalizePhoneNumber(detail.number(), normalizeFlags));
                    if (!normalized.isEmpty()) {
                        insert(matchTokens, makeSearchToken(normalized));
                    }
                }
            }
            wildMatchKeys = toSortedVector(matchTokens);
            return true;
        }

        return false;
    }

    static const QChar *cbegin(const QString &s) { return s.cbegin(); }
    static const QChar *cend(const QString &s) { return s.cend(); }

    static const QChar *cbegin(const QStringRef &r) { return r.data(); }
    static const QChar *cend(const QStringRef &r) { return r.data() + r.size(); }

    template<typename KeyType>
    bool partialMatch(const KeyType &key, const QChar * const vbegin, const QChar * const vend) const
    {
        // Note: both key and value must already be in normalization form D
        const QChar *kbegin = cbegin(key), *kend = cend(key);

        const QChar *vit = vbegin, *kit = kbegin;
        while (kit != kend) {
            if (*kit != *vit)
                break;

            // Preceding base chars match - are there any continuing diacritics?
            QString::const_iterator vmatch = vit++, kmatch = kit++;
            while (vit != vend && (*vit).category() == QChar::Mark_NonSpacing)
                 ++vit;
            while (kit != kend && (*kit).category() == QChar::Mark_NonSpacing)
                 ++kit;

            if ((vit - vmatch) > 1) {
                // The match value contains diacritics - the key needs to match them
                const QString subValue(QString::fromRawData(vmatch, vit - vmatch));
                const QString subKey(QString::fromRawData(kmatch, kit - kmatch));
                if (subValue.compare(subKey) != 0)
                    break;
            } else {
                // Ignore any diacritics in our key
            }

            if (vit == vend) {
                // We have matched to the end of the value
                return true;
            }
        }

        return false;
    }

    bool partialMatch(const QString &value) const
    {
        const QChar *vbegin = value.cbegin(), *vend = value.cend();

        // Find which subset of initial-match keys the value might match
        typedef QVector<const QString *>::const_iterator VectorIterator;
        std::pair<VectorIterator, VectorIterator> bounds = std::equal_range(initialMatchKeys.cbegin(), initialMatchKeys.cend(), vbegin, FirstElementLessThanIndirect());
        for ( ; bounds.first != bounds.second; ++bounds.first) {
            const QString &key(*(*bounds.first));
            if (partialMatch(key, vbegin, vend))
                return true;
        }

        // Test to see if there is a match in any of the match-anywhere fields
        for (QVector<const QString *>::const_iterator it = wildMatchKeys.cbegin(), end = wildMatchKeys.cend(); it != end; ++it) {
            const QString &key(*(*it));

            // Try to match the value anywhere inside the key
            const QChar initialChar(*value.cbegin());
            int index = -1;
            while ((index = key.indexOf(initialChar, index + 1)) != -1) {
                if (partialMatch(key.midRef(index), vbegin, vend))
                    return true;
            }
        }

        return false;
    }

    void itemUpdated(SeasideCache::CacheItem *) { initialMatchKeys.clear(); wildMatchKeys.clear(); }
    void itemAboutToBeRemoved(SeasideCache::CacheItem *) { delete this; }
};

SeasideFilteredModel::SeasideFilteredModel(QObject *parent)
    : SeasideCache::ListModel(parent)
    , m_filterIndex(0)
    , m_referenceIndex(0)
    , m_filterUpdateIndex(-1)
    , m_filterType(FilterAll)
    , m_effectiveFilterType(FilterAll)
    , m_requiredProperty(NoPropertyRequired)
    , m_searchableProperty(NoPropertySearchable)
    , m_searchByFirstNameCharacter(false)
    , m_savePersonActive(false)
    , m_lastItem(0)
    , m_lastId(0)
{
    updateRegistration();

    m_allContactIds = SeasideCache::contacts(SeasideCache::FilterAll);
    m_referenceContactIds = m_allContactIds;
    m_contactIds = m_allContactIds;
    updateSectionBucketIndexCache();
}

SeasideFilteredModel::~SeasideFilteredModel()
{
    SeasideCache::unregisterModel(this);
}

QHash<int, QByteArray> SeasideFilteredModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles.insert(Qt::DisplayRole, displayLabelRole);
    roles.insert(FirstNameRole, firstNameRole);
    roles.insert(LastNameRole, lastNameRole);
    roles.insert(SectionBucketRole, sectionBucketRole);
    roles.insert(FavoriteRole, favoriteRole);
    roles.insert(AvatarRole, avatarRole);
    roles.insert(AvatarUrlRole, avatarUrlRole);
    roles.insert(GlobalPresenceStateRole, globalPresenceStateRole);
    roles.insert(ContactIdRole, contactIdRole);
    roles.insert(PhoneNumbersRole, phoneNumbersRole);
    roles.insert(EmailAddressesRole, emailAddressesRole);
    roles.insert(AccountUrisRole, accountUrisRole);
    roles.insert(AccountPathsRole, accountPathsRole);
    roles.insert(PersonRole, personRole);
    roles.insert(PrimaryNameRole, primaryNameRole);
    roles.insert(SecondaryNameRole, secondaryNameRole);
    roles.insert(NicknameDetailsRole, nicknameDetailsRole);
    roles.insert(PhoneDetailsRole, phoneDetailsRole);
    roles.insert(EmailDetailsRole, emailDetailsRole);
    roles.insert(AccountDetailsRole, accountDetailsRole);
    roles.insert(NoteDetailsRole, noteDetailsRole);
    roles.insert(CompanyNameRole, companyNameRole);
    roles.insert(TitleRole, titleRole);
    roles.insert(RoleRole, roleRole);
    return roles;
}

bool SeasideFilteredModel::isPopulated() const
{
    return SeasideCache::isPopulated(static_cast<SeasideCache::FilterType>(m_filterType));
}

SeasideFilteredModel::DisplayLabelOrder SeasideFilteredModel::displayLabelOrder() const
{
    SeasideCache::DisplayLabelOrder order = SeasideCache::displayLabelOrder();
    return static_cast<SeasideFilteredModel::DisplayLabelOrder>(order);
}

void SeasideFilteredModel::setDisplayLabelOrder(DisplayLabelOrder)
{
    // For compatibility only.
    qWarning() << "PeopleModel::displayLabelOrder is now readonly";
}

QString SeasideFilteredModel::sortProperty() const
{
    return SeasideCache::sortProperty();
}

QString SeasideFilteredModel::groupProperty() const
{
    return SeasideCache::groupProperty();
}

QString SeasideFilteredModel::placeholderDisplayLabel() const
{
    return SeasidePerson::placeholderDisplayLabel();
}

SeasideFilteredModel::FilterType SeasideFilteredModel::filterType() const
{
    return m_filterType;
}

void SeasideFilteredModel::setFilterType(FilterType type)
{
    if (m_filterType != type) {
        const bool wasPopulated = SeasideCache::isPopulated(static_cast<SeasideCache::FilterType>(m_filterType));

        // FilterNone == FilterAll when there is a filter pattern.
        const bool filtered = !m_filterPattern.isEmpty();
        const bool equivalentFilter = (type == FilterAll || type == FilterNone) &&
                                      (m_filterType == FilterAll || m_filterType == FilterNone) &&
                                      filtered;

        m_filterType = type;

        if (!equivalentFilter) {
            m_referenceIndex = 0;
            m_filterIndex = 0;

            m_effectiveFilterType = (m_filterType != FilterNone || m_filterPattern.isEmpty()) ? m_filterType : FilterAll;
            updateRegistration();

            if (!filtered) {
                m_filteredContactIds = *m_referenceContactIds;
            }

            m_referenceContactIds = SeasideCache::contacts(static_cast<SeasideCache::FilterType>(m_filterType));

            updateIndex();

            if (!filtered) {
                m_contactIds = m_referenceContactIds;
                m_filteredContactIds.clear();
            }

            populateSectionBucketIndices();

            if (SeasideCache::isPopulated(static_cast<SeasideCache::FilterType>(m_filterType)) != wasPopulated)
                emit populatedChanged();
        }

        emit filterTypeChanged();
    }
}

QString SeasideFilteredModel::filterPattern() const
{
    return m_filterPattern;
}

void SeasideFilteredModel::setFilterPattern(const QString &pattern)
{
    updateFilters(pattern, m_requiredProperty);
}

int SeasideFilteredModel::requiredProperty() const
{
    return m_requiredProperty;
}

void SeasideFilteredModel::setRequiredProperty(int type)
{
    updateFilters(m_filterPattern, type);
}

int SeasideFilteredModel::searchableProperty() const
{
    return m_searchableProperty;
}

void SeasideFilteredModel::setSearchableProperty(int type)
{
    if (m_searchableProperty != type) {
        m_searchableProperty = type;

        updateRegistration();
        emit searchablePropertyChanged();
    }
}

bool SeasideFilteredModel::searchByFirstNameCharacter() const
{
    return m_searchByFirstNameCharacter;
}

void SeasideFilteredModel::setSearchByFirstNameCharacter(bool searchByFirstNameCharacter)
{
    if (m_searchByFirstNameCharacter != searchByFirstNameCharacter) {
        m_searchByFirstNameCharacter = searchByFirstNameCharacter;
        emit searchByFirstNameCharacterChanged();
    }
}

bool SeasideFilteredModel::filterId(quint32 iid) const
{
    if (m_filterParts.isEmpty() && m_requiredProperty == NoPropertyRequired)
        return true;

    SeasideCache::CacheItem *item = existingItem(iid);
    if (!item)
        return false;

    if (m_requiredProperty != NoPropertyRequired) {
        bool haveMatch = (m_requiredProperty & AccountUriRequired) && (item->statusFlags & SeasideCache::HasValidOnlineAccount);
        haveMatch |= (m_requiredProperty & PhoneNumberRequired) && (item->statusFlags & QContactStatusFlags::HasPhoneNumber);
        haveMatch |= (m_requiredProperty & EmailAddressRequired) && (item->statusFlags & QContactStatusFlags::HasEmailAddress);
        if (!haveMatch)
            return false;
    }

    if (m_searchByFirstNameCharacter && !m_filterPattern.isEmpty())
        return m_filterPattern == SeasideCache::displayLabelGroup(item);

    FilterData *filterData = FilterData::getItemFilterData(item, this);
    filterData->prepareFilter(item);

    // search forwards over the label components for each filter word, making
    // sure to find all filter words before considering it a match.
    for (const QStringList &part : m_filterParts) {
        bool match = false;
        for (const QString &alternative : part) {
            if (filterData->partialMatch(alternative)) {
                match = true;
                break;
            }
        }
        if (!match) {
            return false;
        }
    }

    return true;
}

void SeasideFilteredModel::insertRange(int index, int count, const QList<quint32> &source, int sourceIndex)
{
    beginInsertRows(QModelIndex(), index, index + count - 1);
    for (int i = 0; i < count; ++i)
        m_filteredContactIds.insert(index + i, source.at(sourceIndex + i));
    endInsertRows();
}

void SeasideFilteredModel::removeRange(int index, int count)
{
    beginRemoveRows(QModelIndex(), index, index + count - 1);
    invalidateRows(index, count);
    endRemoveRows();
}

void SeasideFilteredModel::refineIndex()
{
    // The filtered list is a guaranteed sub-set of the current list, so just scan through
    // and remove items that don't match the filter.
    for (int i = 0; i < m_filteredContactIds.count();) {
        int count = 0;
        for (; i + count < m_filteredContactIds.count(); ++count) {
            if (filterId(m_filteredContactIds.at(i + count)))
                break;
        }

        if (count > 0) {
            beginRemoveRows(QModelIndex(), i, i + count - 1);
            invalidateRows(i, count);
            endRemoveRows();
        }
        ++i;
    }
}

void SeasideFilteredModel::updateIndex()
{
    synchronizeFilteredList(this, m_filteredContactIds, *m_referenceContactIds);
}

void SeasideFilteredModel::populateIndex()
{
    // The filtered list is empty, so just scan through the reference list and append any
    // items that match the filter.
    for (int i = 0; i < m_referenceContactIds->count(); ++i) {
        if (filterId(m_referenceContactIds->at(i)))
            m_filteredContactIds.append(m_referenceContactIds->at(i));
    }
    if (!m_filteredContactIds.isEmpty())
        beginInsertRows(QModelIndex(), 0, m_filteredContactIds.count() - 1);

    m_contactIds = &m_filteredContactIds;
    populateSectionBucketIndices();

    if (!m_filteredContactIds.isEmpty()) {
        endInsertRows();
        emit countChanged();
    }
}

QVariantMap SeasideFilteredModel::get(int row) const
{
    SeasideCache::CacheItem *cacheItem = existingItem(m_contactIds->at(row));
    if (!cacheItem)
        return QVariantMap();

    QVariantMap m;
    m.insert(displayLabelRole, data(cacheItem, Qt::DisplayRole));
    m.insert(primaryNameRole, data(cacheItem, PrimaryNameRole));
    m.insert(secondaryNameRole, data(cacheItem, SecondaryNameRole));
    m.insert(firstNameRole, data(cacheItem, FirstNameRole));
    m.insert(lastNameRole, data(cacheItem, LastNameRole));
    m.insert(sectionBucketRole, data(cacheItem, SectionBucketRole));
    m.insert(favoriteRole, data(cacheItem, FavoriteRole));
    m.insert(avatarRole, data(cacheItem, AvatarRole));
    m.insert(avatarUrlRole, data(cacheItem, AvatarUrlRole));
    m.insert(globalPresenceStateRole, data(cacheItem, GlobalPresenceStateRole));
    m.insert(contactIdRole, data(cacheItem, ContactIdRole));
    m.insert(phoneNumbersRole, data(cacheItem, PhoneNumbersRole));
    m.insert(emailAddressesRole, data(cacheItem, EmailAddressesRole));
    m.insert(accountUrisRole, data(cacheItem, AccountUrisRole));
    m.insert(accountPathsRole, data(cacheItem, AccountPathsRole));
    m.insert(nicknameDetailsRole, data(cacheItem, NicknameDetailsRole));
    m.insert(phoneDetailsRole, data(cacheItem, PhoneDetailsRole));
    m.insert(emailDetailsRole, data(cacheItem, EmailDetailsRole));
    m.insert(accountDetailsRole, data(cacheItem, AccountDetailsRole));
    m.insert(noteDetailsRole, data(cacheItem, NoteDetailsRole));
    m.insert(companyNameRole, data(cacheItem, CompanyNameRole));
    m.insert(titleRole, data(cacheItem, TitleRole));
    m.insert(roleRole, data(cacheItem, RoleRole));
    return m;
}

QVariant SeasideFilteredModel::get(int row, int role) const
{
    SeasideCache::CacheItem *cacheItem = existingItem(m_contactIds->at(row));
    if (!cacheItem)
        return QVariant();

    return data(cacheItem, role);
}

bool SeasideFilteredModel::savePerson(SeasidePerson *person)
{
    if (!person) {
        qWarning("savePerson() failed: specified person is null");
        return false;
    }
    if (m_savePersonActive) {
        qWarning("savePerson() is already active, completion signals may arrive out-of-order");
    }
    if (SeasideCache::saveContact(person->contact())) {
        // The actual save is asynchronous, even though the cache will update immediately.
        m_savePersonActive = true;
        // Report that this Person object has changed, since the update
        // resulting from the save will not find any differences
        emit person->dataChanged();
        return true;
    }

    return false;
}

SeasidePerson *SeasideFilteredModel::personByRow(int row) const
{
    if(row < 0 || row >= m_contactIds->size()) {
        return NULL;
    }
    return personFromItem(SeasideCache::itemById(m_contactIds->at(row)));
}

SeasidePerson *SeasideFilteredModel::personById(int id) const
{
    return personFromItem(SeasideCache::itemById(id));
}

SeasidePerson *SeasideFilteredModel::personByPhoneNumber(const QString &number, bool requireComplete) const
{
    return personFromItem(SeasideCache::itemByPhoneNumber(number, requireComplete));
}

SeasidePerson *SeasideFilteredModel::personByEmailAddress(const QString &email, bool requireComplete) const
{
    return personFromItem(SeasideCache::itemByEmailAddress(email, requireComplete));
}

SeasidePerson *SeasideFilteredModel::personByOnlineAccount(const QString &localUid, const QString &remoteUid, bool requireComplete) const
{
    return personFromItem(SeasideCache::itemByOnlineAccount(localUid, remoteUid, requireComplete));
}

SeasidePerson *SeasideFilteredModel::selfPerson() const
{
    return personFromItem(SeasideCache::itemById(SeasideCache::selfContactId()));
}

void SeasideFilteredModel::removePerson(SeasidePerson *person)
{
    SeasideCache::removeContact(person->contact());
}

void SeasideFilteredModel::removePeople(const QVariantList &people)
{
    QList<QContact> contacts;
    for (const QVariant &variant : people) {
        SeasidePerson *person = variant.value<SeasidePerson*>();
        if (person) {
            contacts.append(person->contact());
        }
    }
    SeasideCache::removeContacts(contacts);
}

QModelIndex SeasideFilteredModel::index(const QModelIndex &parent, int row, int column) const
{
    return !parent.isValid() && column == 0 && row >= 0 && row < m_contactIds->count()
            ? createIndex(row, column)
            : QModelIndex();
}

int SeasideFilteredModel::rowCount(const QModelIndex &parent) const
{
    return !parent.isValid()
            ? m_contactIds->count()
            : 0;
}

QVariant SeasideFilteredModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    SeasideCache::CacheItem *cacheItem = existingItem(m_contactIds->at(index.row()));
    if (!cacheItem)
        return QVariant();

    return data(cacheItem, role);
}

QVariant SeasideFilteredModel::data(SeasideCache::CacheItem *cacheItem, int role) const
{
    const QContact &contact = cacheItem->contact;

    if (role == ContactIdRole) {
        return cacheItem->iid;
    } else if (role == PrimaryNameRole || role == SecondaryNameRole) {
        const QContactName nameDetail = contact.detail<QContactName>();

        QString (*fn)(const QString &, const QString &) = (role == PrimaryNameRole ? &SeasideCache::primaryName : &SeasideCache::secondaryName);
        const QString name((*fn)(nameDetail.firstName(), nameDetail.lastName()));

        if (role == PrimaryNameRole && name.isEmpty()) {
            if (SeasideCache::secondaryName(nameDetail.firstName(), nameDetail.lastName()).isEmpty()) {
                // No real name details - fall back to the display label for primary name
                return data(cacheItem, Qt::DisplayRole);
            }
        }
        return name;
    } else if (role == FirstNameRole || role == LastNameRole) {
        // These roles are to be deprecated; users should migrate to primary/secondaryName
        QContactName name = contact.detail<QContactName>();
        return role == FirstNameRole ? name.firstName() : name.lastName();
    } else if (role == FavoriteRole) {
        return contact.detail<QContactFavorite>().isFavorite();
    } else if (role == AvatarRole || role == AvatarUrlRole) {
        QUrl avatarUrl = SeasideCache::filteredAvatarUrl(contact);
        if (role == AvatarUrlRole || !avatarUrl.isEmpty()) {
            return avatarUrl;
        }
        // Return the default avatar path for when no avatar URL is available
        return QUrl(QLatin1String("image://theme/icon-m-telephony-contact-avatar"));
    } else if (role == GlobalPresenceStateRole) {
        QContactGlobalPresence presence = contact.detail<QContactGlobalPresence>();
        return presence.isEmpty()
                ? QContactPresence::PresenceUnknown
                : presence.presenceState();
    } else if (role == NicknameDetailsRole) {
        return SeasidePerson::nicknameDetails(contact);
    } else if (role == PhoneDetailsRole) {
        return SeasidePerson::phoneDetails(contact);
    } else if (role == EmailDetailsRole) {
        return SeasidePerson::emailDetails(contact);
    } else if (role == AccountDetailsRole) {
        return SeasidePerson::accountDetails(contact);
    } else if (role == NoteDetailsRole) {
        return SeasidePerson::noteDetails(contact);
    } else if (role == PhoneNumbersRole || role == EmailAddressesRole || role == AccountUrisRole || role == AccountPathsRole) {
        QStringList rv;
        if (role == PhoneNumbersRole) {
            for (const QContactPhoneNumber &number : contact.details<QContactPhoneNumber>()) {
                rv.append(number.number());
            }
        } else if (role == EmailAddressesRole) {
            for (const QContactEmailAddress &address : contact.details<QContactEmailAddress>()) {
                rv.append(address.emailAddress());
            }
        } else if (role == AccountPathsRole){
            for (const QContactOnlineAccount &account : contact.details<QContactOnlineAccount>()) {
                rv.append(account.value<QString>(QContactOnlineAccount__FieldAccountPath));
            }
        } else {
            for (const QContactOnlineAccount &account : contact.details<QContactOnlineAccount>()) {
                rv.append(account.accountUri());
            }
        }
        return rv;
    } else if (role == CompanyNameRole) {
        return SeasidePerson::companyName(contact);
    } else if (role == TitleRole) {
        return SeasidePerson::title(contact);
    } else if (role == RoleRole) {
        return SeasidePerson::role(contact);
    } else if (role == Qt::DisplayRole || role == SectionBucketRole) {
        if (SeasidePerson *person = static_cast<SeasidePerson *>(cacheItem->itemData)) {
            // If we have a person instance, prefer to use that
            return role == Qt::DisplayRole ? person->displayLabel() : person->sectionBucket();
        }
        return role == Qt::DisplayRole ? cacheItem->displayLabel : cacheItem->displayLabelGroup;
    } else if (role == PersonRole) {
        // Avoid creating a Person instance for as long as possible.
        SeasideCache::ensureCompletion(cacheItem);
        return QVariant::fromValue(personFromItem(cacheItem));
    } else {
        qWarning() << "Invalid role requested:" << role;
    }

    return QVariant();
}

void SeasideFilteredModel::sourceAboutToRemoveItems(int begin, int end)
{
    if (!isFiltered()) {
        beginRemoveRows(QModelIndex(), begin, end);
        invalidateRows(begin, (end - begin) + 1, false, false);
    }
}

void SeasideFilteredModel::sourceItemsRemoved()
{
    if (!isFiltered()) {
        endRemoveRows();
        emit countChanged();
    }
}

void SeasideFilteredModel::sourceAboutToInsertItems(int begin, int end)
{
    if (!isFiltered()) {
        beginInsertRows(QModelIndex(), begin, end);
    }
}

void SeasideFilteredModel::sourceItemsInserted(int begin, int end)
{
    Q_UNUSED(begin)
    Q_UNUSED(end)

    if (!isFiltered()) {
        endInsertRows();
        emit countChanged();
    }
}

void SeasideFilteredModel::sourceDataChanged(int begin, int end)
{
    if (!isFiltered()) {
        emit dataChanged(createIndex(begin, 0), createIndex(end, 0));
    } else {
        // the items inserted/removed notifications arrive sequentially.  All bets are off
        // for dataChanged so we want to reset the progressive indexes back to the beginning.
        m_referenceIndex = 0;
        m_filterIndex = 0;

        // This could be optimised to group multiple changes together, but as of right
        // now begin and end are always the same so theres no point.
        for (int i = begin; i <= end; ++i) {
            const int f = m_filteredContactIds.indexOf(m_referenceContactIds->at(i));
            const bool match = filterId(m_referenceContactIds->at(i));

            if (f < 0 && match) {
                // The contact is not in the filtered list but is a match to the filter; find the
                // correct position and insert it.
                int r = 0;
                int f = 0;
                for (; f < m_filteredContactIds.count(); ++f) {
                    r = m_referenceContactIds->indexOf(m_filteredContactIds.at(f), r);
                    if (r > begin)
                        break;
                }
                beginInsertRows(QModelIndex(), f, f);
                m_filteredContactIds.insert(f, m_referenceContactIds->at(i));
                endInsertRows();
            } else if (f >= 0 && !match) {
                // The contact is in the filtered set but is not a match to the filter; remove it.
                beginRemoveRows(QModelIndex(), f, f);
                invalidateRows(f, 1);
                endRemoveRows();
            } else if (f >= 0) {
                const QModelIndex index = createIndex(f, 0);
                emit dataChanged(index, index);
            }
        }
    }
}

void SeasideFilteredModel::sourceItemsChanged()
{
    if (isFiltered()) {
        const int prevCount = rowCount();

        updateIndex();

        if (rowCount() != prevCount) {
            emit countChanged();
        }
    }
}

void SeasideFilteredModel::makePopulated()
{
    emit populatedChanged();
}

void SeasideFilteredModel::updateDisplayLabelOrder()
{
    emit displayLabelOrderChanged();
}

void SeasideFilteredModel::updateSortProperty()
{
    emit sortPropertyChanged();
}

void SeasideFilteredModel::updateGroupProperty()
{
    emit groupPropertyChanged();
}

void SeasideFilteredModel::updateSectionBucketIndexCache()
{
    populateSectionBucketIndices();
}

void SeasideFilteredModel::saveContactComplete(int localId, int aggregateId)
{
    // This assumes that only one savePerson() call is active at any time
    // which is not guaranteed by the API but which is true in practice.
    if (m_savePersonActive) {
        m_savePersonActive = false;
        if (localId != aggregateId) {
            emit savePersonSucceeded(localId, aggregateId);
        } else {
            // localId = -1, aggregateId = -1 means error.
            emit savePersonFailed();
        }
    }
}

int SeasideFilteredModel::importContacts(const QString &path)
{
    return SeasideCache::importContacts(path);
}

QString SeasideFilteredModel::exportContacts()
{
    return SeasideCache::exportContacts();
}

void SeasideFilteredModel::prepareSearchFilters()
{
    m_filterUpdateIndex = 0;
    updateSearchFilters();
}

void SeasideFilteredModel::populateSectionBucketIndices()
{
    const QStringList allSectionBuckets = SeasideCache::allDisplayLabelGroups();
    QMap<QString, int> firstIndexForSectionBucket;
    for (const QString &bucket : allSectionBuckets) {
        firstIndexForSectionBucket.insert(bucket, -1);
    }

    QString prevHandledSectionBucket;
    for (int i = 0, j = 0; i < m_contactIds->size() && j < allSectionBuckets.size(); ++i) {
        const QString &currSectionBucket(allSectionBuckets[j]);
        SeasideCache::CacheItem *cacheItem = SeasideCache::itemById(m_contactIds->at(i));
        if (!cacheItem) {
            continue;
        }
        if (cacheItem->displayLabelGroup == currSectionBucket) {
            // found the first contact for section bucket j
            firstIndexForSectionBucket.insert(currSectionBucket, i);
            prevHandledSectionBucket = currSectionBucket;
            j++;
        } else if (cacheItem->displayLabelGroup == prevHandledSectionBucket) {
            // another contact in the most-recently-handled bucket.  skip it.
            continue;
        } else if (allSectionBuckets.indexOf(cacheItem->displayLabelGroup) < j) {
            // We expect contacts to be ordered by display label group
            // so if we hit a contact whose dlg is LESS than the currSectionBucket
            // we know we have out-of-order data.
            // This can happen after we have saved a contact, for example, as
            // we then fetch the contact by id and immediately update
            // even though the order of that contact won't have been
            // updated according to the (potentially new) dlg sort order.
            // So, early exit and wait for the refresh request (re-sort)
            // to finish and trigger this method again.
            return;
        } else if (cacheItem->displayLabelGroup != prevHandledSectionBucket) {
            // no contact exists which has this section bucket.
            prevHandledSectionBucket = currSectionBucket;
            j++;
            i--;
        }
    }

    m_firstIndexForSectionBucket = firstIndexForSectionBucket;
}

int SeasideFilteredModel::firstIndexInGroup(const QString &sectionBucket)
{
    if (sectionBucket.isEmpty()) {
        qWarning() << "group name is empty!";
        return -1;
    }

    bool returnNextValidIndex = false;
    const QStringList sectionBuckets = m_firstIndexForSectionBucket.keys();
    for (int i = sectionBuckets.size() - 1; i >= 0; --i) {
        const QString &currSectionBucket(sectionBuckets[i]);
        const int firstIndexInCurrSectionBucket = m_firstIndexForSectionBucket.value(currSectionBucket);
        if (currSectionBucket == sectionBucket) {
            if (firstIndexInCurrSectionBucket >= 0) {
                return firstIndexInCurrSectionBucket;
            } else {
                // There are no contact in this group, so return the index
                // of the first contact in the previous (populated) group.
                returnNextValidIndex = true;
            }
        } else if (firstIndexInCurrSectionBucket >= 0 && returnNextValidIndex) {
            return firstIndexInCurrSectionBucket;
        }
    }

    return -1;
}

SeasidePerson *SeasideFilteredModel::personFromItem(SeasideCache::CacheItem *item) const
{
    if (!item)
        return 0;

    if (!item->itemData) {
        item->itemData = new SeasidePerson(&item->contact, (item->contactState == SeasideCache::ContactComplete), SeasideCache::instance());
    }

    return static_cast<SeasidePerson *>(item->itemData);
}

bool SeasideFilteredModel::isFiltered() const
{
    return m_effectiveFilterType != FilterNone && (!m_filterPattern.isEmpty() || (m_requiredProperty != NoPropertyRequired));
}

void SeasideFilteredModel::updateFilters(const QString &pattern, int property)
{
    if ((pattern == m_filterPattern) && (property == m_requiredProperty))
        return;

    const bool filtered = isFiltered();
    const bool removeFilter = pattern.isEmpty() && property == NoPropertyRequired;
    const bool refinement = (pattern == m_filterPattern || pattern.startsWith(m_filterPattern, Qt::CaseInsensitive)) &&
                            (property == m_requiredProperty || m_requiredProperty == NoPropertyRequired);

    const int prevCount = rowCount();

    bool changedPattern(false);
    bool changedProperty(false);

    if (m_filterPattern != pattern) {
        m_filterPattern = pattern;
        m_filterParts = extractSearchTerms(m_filterPattern);
        // Qt5 does not recognize '#' as a word
        if (m_filterParts.isEmpty() && !pattern.isEmpty()) {
            m_filterParts.append(QStringList() << pattern);
        }
        changedPattern = true;
    }
    if (m_requiredProperty != property) {
        m_requiredProperty = property;
        changedProperty = true;

        // Update our registration to include the data type we need
        updateRegistration();
    }

    m_referenceIndex = 0;
    m_filterIndex = 0;

    if (m_filterType == FilterNone && m_effectiveFilterType == FilterNone && !m_filterPattern.isEmpty()) {
        // Start showing filtered results
        m_effectiveFilterType = FilterAll;
        updateRegistration();

        m_referenceContactIds = m_allContactIds;
        populateIndex();
    } else if (m_filterType == FilterNone && m_effectiveFilterType == FilterAll && m_filterPattern.isEmpty()) {
        // We should no longer show any results
        m_effectiveFilterType = FilterNone;
        updateRegistration();

        const bool hadMatches = m_contactIds->count() > 0;
        if (hadMatches) {
            beginRemoveRows(QModelIndex(), 0, m_contactIds->count() - 1);
            invalidateRows(0, m_contactIds->count(), true, false);
        }

        m_referenceContactIds = SeasideCache::contacts(SeasideCache::FilterNone);
        m_contactIds = m_referenceContactIds;
        m_filteredContactIds.clear();
        populateSectionBucketIndices();

        if (hadMatches) {
            endRemoveRows();
        }
    } else if (!filtered) {
        m_filteredContactIds = *m_referenceContactIds;
        m_contactIds = &m_filteredContactIds;
        populateSectionBucketIndices();

        refineIndex();
    } else if (refinement) {
        refineIndex();
    } else {
        updateIndex();

        if (removeFilter) {
            m_contactIds = m_referenceContactIds;
            m_filteredContactIds.clear();
            populateSectionBucketIndices();
        }
    }

    if (rowCount() != prevCount) {
        emit countChanged();
    }
    if (changedPattern) {
        emit filterPatternChanged();
    }
    if (changedProperty) {
        emit requiredPropertyChanged();
    }
}

void SeasideFilteredModel::updateRegistration()
{
    const SeasideCache::FetchDataType requiredTypes(static_cast<SeasideCache::FetchDataType>(m_requiredProperty));
    const SeasideCache::FetchDataType extraTypes(static_cast<SeasideCache::FetchDataType>(m_searchableProperty));
    SeasideCache::registerModel(this, static_cast<SeasideCache::FilterType>(m_effectiveFilterType), requiredTypes, extraTypes);
}

void SeasideFilteredModel::invalidateRows(int begin, int count, bool filteredIndex, bool removeFromModel)
{
    const QList<quint32> *contactIds(filteredIndex ? &m_filteredContactIds : m_referenceContactIds);

    for (int index = begin; index < (begin + count); ++index) {
        if (contactIds->at(index) == m_lastId) {
            m_lastId = 0;
            m_lastItem = 0;
        }
    }

    if (removeFromModel) {
        Q_ASSERT(filteredIndex);
        QList<quint32>::iterator it = m_filteredContactIds.begin() + begin;
        m_filteredContactIds.erase(it, it + count);
    }
}

SeasideCache::CacheItem *SeasideFilteredModel::existingItem(quint32 iid) const
{
    // Cache the last item lookup - repeated property lookups will be for the same index
    if (iid != m_lastId) {
        m_lastId = iid;
        m_lastItem = SeasideCache::existingItem(m_lastId);
    }
    return m_lastItem;
}

void SeasideFilteredModel::updateSearchFilters()
{
    static const int maxBatchSize = 100;

    for (int n = 0; m_filterUpdateIndex < m_allContactIds->count(); ++m_filterUpdateIndex) {
        SeasideCache::CacheItem *item = existingItem(m_allContactIds->at(m_filterUpdateIndex));
        if (!item)
            continue;

        FilterData *filterData = FilterData::getItemFilterData(item, this);
        if (filterData->prepareFilter(item)) {
            if (++n == maxBatchSize) {
                // Schedule further processing
                QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
                return;
            }
        }
    }

    // Nothing left to process
    m_filterUpdateIndex = -1;
}

bool SeasideFilteredModel::event(QEvent *event)
{
    if (event->type() != QEvent::UpdateRequest)
        return QObject::event(event);

    if (m_filterUpdateIndex != -1) {
        updateSearchFilters();
    }

    return true;
}

