#pragma once
#include "ProfileRecord.hxx"
#include <nlohmann/json.hpp>
#include <utility>

namespace sf4e { namespace netplay {
inline nlohmann::json ProfileRecordJson(const ProfileRecord& record) {
    return {{"wins",record.wins},{"losses",record.losses},{"recent",record.recent}};
}
inline bool ReadProfileRecord(const nlohmann::json& value, ProfileRecord& record) {
    try {
        if(!value.is_object()||!value.at("wins").is_number_unsigned()||!value.at("losses").is_number_unsigned()||
            !value.at("recent").is_array())throw 0;
        ProfileRecord next;next.wins=value.at("wins").get<std::uint64_t>();next.losses=value.at("losses").get<std::uint64_t>();next.recent=value.at("recent").get<std::deque<std::string>>();
    if(next.wins>ProfileRecord::MaximumGames||next.losses>ProfileRecord::MaximumGames||
       next.losses>ProfileRecord::MaximumGames-next.wins||next.recent.size()>ProfileRecord::MaximumRecentEntries)throw 0;
        // Legacy epoch-based strings are intentionally opaque and remain
        // readable. New strings are v2 keys, but both share the same bounded
        // storage budget. Preserve even opaque legacy strings verbatim.
        for(const auto& key:next.recent)
            if(key.size()>ProfileRecord::MaximumRecentKeyBytes)throw 0;
        // Assign the bounded fields explicitly.  This keeps the reader's
        // success path independent of any future metadata added to the
        // record value type and makes the decoded counters observable to
        // callers that retain an existing record object.
        record.wins=next.wins;record.losses=next.losses;record.recent=std::move(next.recent);record.available=true;return true;
    }catch(...){record.available=false;return false;}
}
} }
