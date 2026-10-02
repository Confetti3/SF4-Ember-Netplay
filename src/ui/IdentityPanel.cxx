#include "IdentityPanel.hxx"
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include "../common/TournamentLink.hxx"
#include <imgui.h>
#include <algorithm>
#include <cstring>

namespace sf4e { namespace ui {
namespace {
using netplay::IdentityOp;
using netplay::IdentityRequest;
// An answer gets this long before the panel stops waiting for it. Key work
// takes seconds; a bridge request is bounded by the helper's own timeouts.
constexpr double AnswerSeconds = 120;

// The helper's and the bridge's failure codes, in words. Codes that mean the
// same thing to the player share a sentence.
std::string FailureText(const std::string& code) {
    static const std::pair<const char*, const char*> table[] = {
        {"wrong_passphrase", "identity.failure.wrong_passphrase"}, {"decrypt_failed", "identity.failure.wrong_passphrase"},
        {"invalid_passphrase", "identity.failure.invalid_passphrase"},
        {"passphrase_required", "identity.failure.passphrase_required"},
        {"corrupt", "identity.failure.corrupt"}, {"unsupported_envelope", "identity.failure.corrupt"},
        {"file_unreadable", "identity.failure.file"}, {"invalid_path", "identity.failure.file"},
        {"identity_busy", "identity.failure.busy"},
        {"identity_unavailable", "identity.failure.state"}, {"wrong_state", "identity.failure.state"},
        {"key_missing", "identity.failure.state"},
        {"confirmation_mismatch", "identity.failure.changed"}, {"replacement_not_confirmed", "identity.failure.changed"},
        {"permission_denied", "identity.failure.storage"}, {"io", "identity.failure.storage"},
        {"random_unavailable", "identity.failure.storage"}, {"metadata_mismatch", "identity.failure.storage"},
        {"bridge_unreachable", "identity.failure.unreachable"}, {"service_unavailable", "identity.failure.unreachable"},
        {"not_a_bridge", "identity.failure.not_bridge"}, {"invalid_origin", "identity.failure.not_bridge"},
        {"unsupported_bridge", "identity.failure.not_bridge"}, {"invalid_bridge", "identity.failure.not_bridge"},
        {"bridge_invalid_response", "identity.failure.not_bridge"}, {"bridge_response_too_large", "identity.failure.not_bridge"},
        {"unsupported_version", "identity.failure.not_bridge"},
        {"bridge_changed", "identity.failure.bridge_changed"}, {"bridge_mismatch", "identity.failure.bridge_changed"},
        {"bridge_not_approved", "identity.failure.not_approved"},
        {"too_many_bridges", "identity.failure.too_many"},
        {"link_expired", "identity.failure.code"}, {"not_found", "identity.failure.code"},
        {"link_conflict", "identity.failure.link_conflict"},
        {"rate_limited", "identity.failure.rate_limited"},
        {"challenge_mismatch", "identity.failure.sign_in"}, {"invalid_signature", "identity.failure.sign_in"},
        {"challenge_expired", "identity.failure.sign_in"}, {"challenge_used", "identity.failure.sign_in"},
        {"unauthenticated", "identity.failure.sign_in"},
        {"forbidden", "identity.failure.forbidden"}, {"invalid_request", "identity.failure.refused"},
    };
    for (const auto& entry : table) if (code == entry.first) return loc::T(entry.second);
    return loc::Tf("identity.failure.other", code.empty() ? std::string("?") : code);
}
bool Playing(const netplay::tournament::Status& t) {
    using netplay::tournament::Phase;
    return t.phase == Phase::Claiming || t.phase == Phase::Opening || t.phase == Phase::InRoom;
}
std::string PlayState(const netplay::tournament::Status& t) {
    using netplay::tournament::Phase;
    switch (t.phase) {
    case Phase::Claiming: return loc::T("tournament.state.claiming");
    case Phase::Opening: return loc::T("tournament.state.opening");
    case Phase::InRoom:
        return t.waitingForPermit ? loc::T("tournament.state.waiting_permit") :
            t.waitingForOpponent ? loc::T("tournament.state.waiting_opponent") : loc::T("tournament.state.in_room");
    case Phase::Finished: return loc::T("tournament.state.finished");
    case Phase::Failed: return loc::T("tournament.state.failed");
    default: return {};
    }
}
const char* SecretValue(const std::string& secret) {
    return secret.empty() ? loc::T("identity.secret_empty") : loc::T("identity.secret_set");
}
MenuEntry Secret(std::string id, std::string label, const std::string& secret, std::string detail, bool enabled) {
    auto e = TextRow(std::move(id), std::move(label), SecretValue(secret), 256, enabled);
    e.secret = true; e.detail = std::move(detail); return e;
}
MenuEntry Info(std::string id, std::string label, std::string value, std::string detail) {
    auto e = Row(std::move(id), std::move(label), std::move(detail)); e.value = std::move(value); e.info = true; return e;
}
// The two passphrase fields agree and are not empty.
bool Matching(const std::string& a, const std::string& b) { return !a.empty() && a == b; }
std::string PairDetail(const std::string& a, const std::string& b, const char* ready) {
    return !a.empty() && !b.empty() && a != b ? loc::T("identity.passphrases_differ") : loc::T(ready);
}
void Assign(std::string& target, const std::string& text) { WipeText(target); target = text; }
const netplay::IdentityBridge* FindBridge(const ShellView& v, const std::string& id) {
    const auto it = std::find_if(v.identity.bridges.begin(), v.identity.bridges.end(),
        [&](const netplay::IdentityBridge& bridge) { return bridge.id == id; });
    return it == v.identity.bridges.end() ? nullptr : &*it;
}
}

bool IdentityPanel::Owns(const std::string& screen) {
    return screen == "identity" || screen == "identity-backup" || screen == "linked-accounts" || screen == "tournament-matches";
}

void IdentityPanel::OpenMatch(const std::string& bridge, const std::string& match) {
    bridge_ = bridge;
    wantAssignments_ = true;
    opened_ = OpenedMatch{bridge, "tm-play:" + match, std::nullopt};
    // The fresh service list says whether the player trusts this one.
    IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list));
}

std::string IdentityPanel::TakeFocus(const std::vector<MenuEntry>& rows) {
    if (!opened_ || std::none_of(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == opened_->row; })) return {};
    const std::string row = opened_->row;
    opened_.reset();
    return row;
}

void IdentityPanel::CheckOpenedMatch(const netplay::tournament::AssignmentList& list) {
    if (!opened_ || !opened_->finishedBefore || list.finished == *opened_->finishedBefore || list.bridge != opened_->bridge) return;
    const bool listed = std::any_of(list.items.begin(), list.items.end(),
        [&](const netplay::tournament::Assignment& item) { return "tm-play:" + item.matchId == opened_->row; });
    if (listed) return;
    // A failed refresh was said already.
    if (list.error.empty()) Say(loc::T("tournament.failure.not_yours"), true, 12);
    opened_.reset();
}

std::string IdentityPanel::TournamentFailure(const std::string& code) {
    if (code == "incompatible_build") return loc::T("tournament.failure.build");
    if (code == "unsupported_rules") return loc::T("tournament.failure.rules");
    if (code == "helper_lost" || code == "helper_unavailable") return loc::T("tournament.failure.helper");
    if (code == "report_not_saved") return loc::T("tournament.failure.report");
    if (code == "not_found") return loc::T("tournament.failure.not_found");
    return FailureText(code);
}

bool IdentityPanel::Answered(const ShellView& v) const {
    return v.identityTicket == sent_ && (!v.identityRequest || v.identity.requestId == v.identityRequest);
}
bool IdentityPanel::Busy(const ShellView&) const { return sent_ != 0 || !queue_.empty(); }

void IdentityPanel::Say(std::string text, bool error, double seconds) {
    message_ = std::move(text); messageError_ = error; messageUntil_ = now_ + seconds;
}

void IdentityPanel::Queue(IdentityRequest request) {
    if (queue_.size() < 8) queue_.push_back(std::move(request));
}

void IdentityPanel::Wipe() {
    for (auto* secret : {&newPassphrase_, &newConfirm_, &backupPassphrase_, &backupConfirm_, &restorePassphrase_}) WipeText(*secret);
}

void IdentityPanel::Refresh(const ShellView&, const std::string& screen) {
    IdentityRequest status; status.op = IdentityOp::Status; Queue(std::move(status));
    if (screen == "linked-accounts" || screen == "tournament-matches") { IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list)); }
    if (screen == "tournament-matches") wantAssignments_ = true;
}

void IdentityPanel::SelectBridge(const ShellView& v, const std::string& id) {
    bridge_ = id;
    const auto* bridge = FindBridge(v, id);
    if (!bridge) return;
    // The service's connections come from its profile; its links from the list.
    IdentityRequest inspect; inspect.op = IdentityOp::BridgeInspect; inspect.origin = bridge->origin; Queue(std::move(inspect));
    IdentityRequest list; list.op = IdentityOp::LinkList; list.bridge = id; Queue(std::move(list));
}

void IdentityPanel::Update(const ShellView& v, const std::string& screen, const Submit& submit, double now) {
    now_ = now;
    const bool owned = Owns(screen);
    if (!owned && onScreens_) Wipe();
    if (owned && screen != lastScreen_) Refresh(v, screen);
    onScreens_ = owned; lastScreen_ = screen;
    if (sent_ && Answered(v)) { Finish(v); sent_ = 0; }
    else if (sent_ && now - sentAt_ > AnswerSeconds) {
        Say(loc::T("identity.failure.timeout"), true); sent_ = 0; queue_.clear();
    }
    SendTournament(v, screen, submit);
    if (sent_ || queue_.empty()) return;
    ShellAction action;
    action.command.generation = v.session.generation;
    action.identity = std::move(queue_.front()); queue_.pop_front();
    action.identity.ticket = ++nextTicket_;
    const auto ticket = action.identity.ticket;
    sentOp_ = action.identity.op; sentBridge_ = action.identity.bridge;
    if (submit(std::move(action))) { sent_ = ticket; sentAt_ = now; }
    else { Say(loc::T("error.queue_failed"), true); queue_.clear(); }
}

void IdentityPanel::SendTournament(const ShellView& v, const std::string& screen, const Submit& submit) {
    using netplay::tournament::Command;
    using netplay::tournament::Phase;
    const auto& t = v.tournament;
    loadingAssignments_ = t.list.loading;
    CheckOpenedMatch(t.list);
    // A refresh's failure is said once; the runtime clears it when the next one is sent.
    if (t.list.error != assignmentsError_) {
        assignmentsError_ = t.list.error;
        if (!assignmentsError_.empty() && screen == "tournament-matches") Say(TournamentFailure(assignmentsError_), true);
    }
    // A match that ended has a new score to show.
    if (t.phase != phase_) {
        if (t.phase == Phase::Finished || t.phase == Phase::Failed) wantAssignments_ = true;
        phase_ = t.phase;
    }
    ShellAction action;
    action.command.generation = v.session.generation;
    if (play_) {
        action.tournament = std::move(*play_);
        play_.reset();
    } else if (wantAssignments_ && screen == "tournament-matches" && FindBridge(v, bridge_) && !t.list.loading) {
        wantAssignments_ = false;
        action.tournament.op = Command::Op::Refresh;
        action.tournament.bridgeId = bridge_;
        if (opened_ && opened_->bridge == bridge_) opened_->finishedBefore = t.list.finished;
    } else {
        return;
    }
    if (!submit(std::move(action))) Say(loc::T("error.queue_failed"), true);
}

MenuEntry IdentityPanel::ServiceRow(const ShellView& v, const netplay::IdentityBridge& bridge) const {
    auto service = Row("id-bridge", loc::T("identity.service"), bridge.origin);
    service.value = bridge.name.empty() ? bridge.origin : bridge.name; service.userText = true;
    for (const auto& choice : v.identity.bridges) service.choices.push_back({choice.id, choice.name.empty() ? choice.origin : choice.name, choice.origin});
    service.chosen = bridge_;
    if (service.choices.size() < 2) service.info = true;
    return service;
}

void IdentityPanel::Finish(const ShellView& v) {
    const auto& id = v.identity;
    if (!v.identityRequest) {
        Say(v.identityRefusal.empty() ? std::string(loc::T("identity.refused.helper")) : std::string(loc::T(v.identityRefusal.c_str())), true);
        queue_.clear(); return;
    }
    if (!id.ok) {
        // A link list on a service the helper no longer trusts just ends the selection.
        if (sentOp_ == IdentityOp::LinkList && id.failure == "bridge_not_approved") { bridge_.clear(); listedBridge_.clear(); return; }
        // The site approved or refused the request before the cancellation
        // arrived: say so and show what it decided.
        if (sentOp_ == IdentityOp::LinkCancel && id.failure == "link_conflict") {
            Say(loc::T("identity.failure.already_answered"), true);
            queue_.clear();
            IdentityRequest list; list.op = IdentityOp::LinkList; list.bridge = bridge_; Queue(std::move(list));
            return;
        }
        Say(FailureText(id.failure), true); queue_.clear(); return;
    }
    switch (sentOp_) {
    case IdentityOp::Enable: WipeText(newPassphrase_); WipeText(newConfirm_); Say(loc::T("identity.done.enabled"), false); break;
    case IdentityOp::Unlock: Say(loc::T("identity.done.unlocked"), false); break;
    case IdentityOp::Export:
        WipeText(backupPassphrase_); WipeText(backupConfirm_);
        Say(loc::Tf("identity.done.exported", id.exportPath), false, 10); break;
    case IdentityOp::PreviewImport: previewPath_ = restorePath_; break;
    case IdentityOp::Import:
        WipeText(restorePassphrase_); WipeText(newPassphrase_); WipeText(newConfirm_); previewPath_.clear();
        Say(loc::T("identity.done.imported"), false); break;
    case IdentityOp::Reset: previewPath_.clear(); Say(loc::T("identity.done.reset"), false); break;
    case IdentityOp::BridgeList:
        if (opened_ && !FindBridge(v, opened_->bridge)) { Say(loc::T("tournament.failure.link_service"), true, 12); opened_.reset(); }
        if (!FindBridge(v, bridge_)) bridge_ = id.bridges.empty() ? std::string() : id.bridges.front().id;
        if (!bridge_.empty()) SelectBridge(v, bridge_);
        break;
    case IdentityOp::BridgeInspect:
        if (lookingUp_) { found_ = id.inspected; lookingUp_ = false; }
        if (id.inspected.id == bridge_) {
            const bool known = std::any_of(id.connections.begin(), id.connections.end(),
                [&](const netplay::IdentityConnection& c) { return c.id == connection_; });
            if (!known) connection_ = id.connections.empty() ? std::string() : id.connections.front().id;
        }
        break;
    case IdentityOp::BridgeApprove: {
        Say(loc::Tf("identity.done.trusted", found_.name), false);
        bridge_ = found_.id; found_ = {}; WipeText(origin_);
        IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list));
        break;
    }
    case IdentityOp::BridgeForget: {
        Say(loc::T("identity.done.forgotten"), false); bridge_.clear(); listedBridge_.clear();
        IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list));
        break;
    }
    case IdentityOp::LinkList: listedBridge_ = sentBridge_; break;
    case IdentityOp::LinkClaim: {
        code_.clear();
        Say(loc::Tf("identity.done.claimed", id.claimFingerprint), false, 20);
        IdentityRequest list; list.op = IdentityOp::LinkList; list.bridge = bridge_; Queue(std::move(list));
        break;
    }
    case IdentityOp::LinkCancel: case IdentityOp::LinkRemove: {
        Say(loc::T(sentOp_ == IdentityOp::LinkRemove ? "identity.done.unlinked" : "identity.done.cancelled"), false);
        IdentityRequest list; list.op = IdentityOp::LinkList; list.bridge = bridge_; Queue(std::move(list));
        break;
    }
    default: break;
    }
}

bool IdentityPanel::Status(std::string& status, Tone& tone, double now) const {
    if (!message_.empty() && now < messageUntil_) { status = message_; tone = messageError_ ? Tone::Error : Tone::Success; return true; }
    if (sent_ || !queue_.empty()) {
        const bool slow = sentOp_ == IdentityOp::Enable || sentOp_ == IdentityOp::Unlock || sentOp_ == IdentityOp::Export ||
            sentOp_ == IdentityOp::PreviewImport || sentOp_ == IdentityOp::Import;
        status = loc::T(slow ? "identity.working_key" : "identity.working"); tone = Tone::Pending; return true;
    }
    if (lastScreen_ == "tournament-matches" && loadingAssignments_) { status = loc::T("tournament.loading"); tone = Tone::Pending; return true; }
    return false;
}

std::vector<MenuEntry> IdentityPanel::Rows(const ShellView& v, const std::string& screen, std::string& title) const {
    std::vector<MenuEntry> rows;
    const auto& id = v.identity;
    const bool busy = sent_ != 0 || !queue_.empty();
    const bool ready = id.state == "ready";
    const bool canEdit = v.canEditPreferences;
    const auto local = [&](std::vector<MenuEntry>& out) {
        // Under Wine and Proton the key on this PC needs a passphrase of its own.
        out.push_back(Secret("id-new-passphrase", loc::T("identity.local_passphrase"), newPassphrase_,
            loc::T("identity.local_passphrase_detail"), true));
        out.push_back(Secret("id-new-confirm", loc::T("identity.repeat_passphrase"), newConfirm_,
            PairDetail(newPassphrase_, newConfirm_, "identity.repeat_passphrase_detail"), true));
    };
    if (screen == "identity") {
        title = loc::T("identity.title");
        std::string value, detail;
        if (!id.known) { value = loc::T("identity.state.checking"); detail = loc::T("identity.state.checking_detail"); }
        else if (id.state == "disabled") { value = loc::T("common.off"); detail = loc::T("identity.state.disabled_detail"); }
        else if (id.state == "creating") { value = loc::T("identity.state.creating"); detail = loc::T("identity.state.creating_detail"); }
        else if (id.state == "locked") { value = loc::T("identity.state.locked"); detail = loc::T("identity.state.locked_detail"); }
        else if (ready) { value = id.fingerprint; detail = loc::Tf("identity.state.ready_detail", id.emberId, id.fingerprint); }
        else if (id.state == "recovery_required") { value = loc::T("identity.state.recovery"); detail = loc::T("identity.state.recovery_detail"); }
        else { value = loc::T("identity.state.unavailable"); detail = loc::Tf("identity.state.unavailable_detail", FailureText(id.reason)); }
        rows.push_back(Info("id-status", loc::T("identity.status"), value, detail));
        if (id.state == "disabled") {
            if (id.passphraseRequired) local(rows);
            const bool secretOk = !id.passphraseRequired || Matching(newPassphrase_, newConfirm_);
            rows.push_back(ConfirmRow("id-enable", loc::T("identity.enable"),
                canEdit ? loc::T("identity.enable_detail") : loc::T("identity.leave_room"), canEdit && !busy && secretOk));
        }
        if (id.state == "locked") {
            rows.push_back(Secret("id-unlock", loc::T("identity.unlock"), {}, loc::T("identity.unlock_detail"), true));
            rows.back().hint = loc::T("identity.unlock");
        }
        if (ready) {
            rows.push_back(Row("id-copy", loc::T("identity.copy"), loc::T("identity.copy_detail")));
            rows.push_back(Row("linked-accounts", loc::T("screen.linked_accounts"), loc::T("identity.linked_detail")));
            rows.push_back(Row("tournament-matches", loc::T("screen.tournament_matches"), loc::T("identity.matches_detail")));
        }
        rows.push_back(Row("identity-backup", loc::T("screen.identity_backup"),
            ready ? loc::T("identity.backup_detail") : loc::T("identity.restore_detail"), id.known && id.state != "creating"));
        if (id.state == "locked" || id.state == "unavailable" || id.state == "recovery_required")
            rows.push_back(ConfirmRow("id-reset", loc::T("identity.reset"),
                canEdit ? loc::T("identity.reset_detail") : loc::T("identity.leave_room"), canEdit && !busy));
    } else if (screen == "identity-backup") {
        title = loc::T("identity.backup_title");
        if (ready) {
            rows.push_back(Secret("id-backup-passphrase", loc::T("identity.backup_passphrase"), backupPassphrase_,
                loc::T("identity.backup_passphrase_detail"), true));
            rows.push_back(Secret("id-backup-confirm", loc::T("identity.repeat_passphrase"), backupConfirm_,
                PairDetail(backupPassphrase_, backupConfirm_, "identity.repeat_passphrase_detail"), true));
            rows.push_back(ConfirmRow("id-export", loc::T("identity.export"),
                PairDetail(backupPassphrase_, backupConfirm_, "identity.export_detail"), !busy && Matching(backupPassphrase_, backupConfirm_)));
            if (!id.exportPath.empty()) rows.push_back(Info("id-export-result", loc::T("identity.export_saved"), {}, id.exportPath));
        }
        rows.push_back(Row("id-restore-paste", loc::T("identity.paste_path"), loc::T("identity.paste_path_detail")));
        rows.back().hint = loc::T("menu.hint.paste");
        auto path = TextRow("id-restore-path", loc::T("identity.backup_file"), restorePath_, 1024);
        path.detail = loc::T("identity.backup_file_detail");
        rows.push_back(std::move(path));
        rows.push_back(Secret("id-restore-passphrase", loc::T("identity.backup_passphrase"), restorePassphrase_,
            loc::T("identity.restore_passphrase_detail"), true));
        rows.push_back(Row("id-preview", loc::T("identity.check_backup"), loc::T("identity.check_backup_detail"),
            !busy && !restorePath_.empty() && !restorePassphrase_.empty()));
        if (!previewPath_.empty() && previewPath_ == restorePath_ && !id.previewEmberId.empty()) {
            // A locked ID restored from its own backup gets a new passphrase on
            // this PC, which is how a forgotten one is recovered.
            const bool alreadyHere = id.previewSame && ready;
            std::string holds = loc::Tf("identity.backup_holds_detail", id.previewEmberId);
            holds += "\n\n";
            holds += alreadyHere ? loc::T("identity.backup_same") : id.previewSame ? loc::T("identity.backup_same_locked") :
                id.previewReplaces ? loc::T("identity.backup_replaces") : loc::T("identity.backup_new");
            rows.push_back(Info("id-preview-result", loc::T("identity.backup_holds"), id.previewFingerprint, holds));
            if (!alreadyHere) {
                if (id.passphraseRequired) local(rows);
                const bool secretOk = !id.passphraseRequired || Matching(newPassphrase_, newConfirm_);
                rows.push_back(ConfirmRow("id-import", loc::T("identity.import"),
                    !canEdit ? loc::T("identity.leave_room") : id.previewReplaces ? loc::T("identity.import_replaces_detail") : loc::T("identity.import_detail"),
                    canEdit && !busy && secretOk && !restorePassphrase_.empty()));
            }
        }
    } else if (screen == "linked-accounts") {
        title = loc::T("identity.linked_title");
        if (!ready) {
            rows.push_back(Info("id-linked-unavailable", loc::T("identity.linked_needs_id"), {}, loc::T("identity.linked_needs_id_detail")));
            return rows;
        }
        if (const auto* bridge = FindBridge(v, bridge_)) {
            rows.push_back(ServiceRow(v, *bridge));
            if (listedBridge_ == bridge_) {
                for (const auto& link : id.links) {
                    auto row = ConfirmRow("id-unlink:" + link.id, link.account.empty() ? link.provider : link.provider + ": " + link.account,
                        loc::T("identity.unlink_detail"), !busy);
                    row.userText = true; row.hint = loc::T("identity.unlink"); rows.push_back(std::move(row));
                }
                for (const auto& claim : id.pending) {
                    auto row = ConfirmRow("id-cancel:" + claim.id, loc::Tf("identity.pending_label", claim.provider),
                        loc::T("identity.pending_detail"), !busy);
                    // The provider's name comes from the service and may be elided.
                    row.userText = true; row.hint = loc::T("identity.cancel_claim"); rows.push_back(std::move(row));
                }
                if (id.links.empty() && id.pending.empty())
                    rows.push_back(Info("id-no-links", loc::T("identity.no_links"), {}, loc::T("identity.no_links_detail")));
            }
            if (id.inspected.id == bridge_ && id.connections.size() > 1) {
                auto connection = Row("id-connection", loc::T("identity.site"), loc::T("identity.site_detail"));
                for (const auto& choice : id.connections) {
                    connection.choices.push_back({choice.id, choice.name.empty() ? choice.id : choice.name, {}});
                    if (choice.id == connection_) connection.value = connection.choices.back().label;
                }
                connection.chosen = connection_; connection.userText = true;
                rows.push_back(std::move(connection));
            }
            auto code = TextRow("id-code", loc::T("identity.link_code"), code_, 16);
            code.detail = loc::T("identity.link_code_detail");
            rows.push_back(std::move(code));
            rows.push_back(Row("id-claim", loc::T("identity.link_account"), loc::T("identity.link_account_detail"),
                !busy && !code_.empty() && !connection_.empty()));
            rows.push_back(Row("id-refresh", loc::T("identity.refresh"), loc::T("identity.refresh_detail"), !busy));
            rows.push_back(ConfirmRow("id-forget", loc::T("identity.forget"), loc::T("identity.forget_detail"), !busy));
        }
        auto address = TextRow("id-origin", loc::T("identity.service_address"), origin_, 256);
        address.detail = loc::T("identity.service_address_detail");
        rows.push_back(std::move(address));
        rows.push_back(Row("id-lookup", loc::T("identity.look_up"), loc::T("identity.look_up_detail"), !busy && !origin_.empty()));
        if (!found_.id.empty() && !FindBridge(v, found_.id)) {
            auto trust = ConfirmRow("id-approve", loc::Tf("identity.trust", found_.name.empty() ? found_.origin : found_.name),
                loc::Tf("identity.trust_detail", found_.origin), !busy);
            trust.userText = true; rows.push_back(std::move(trust));
        }
    } else if (screen == "tournament-matches") {
        title = loc::T("screen.tournament_matches");
        if (!ready) {
            rows.push_back(Info("id-linked-unavailable", loc::T("identity.linked_needs_id"), {}, loc::T("identity.linked_needs_id_detail")));
            return rows;
        }
        const auto& t = v.tournament;
        const bool playing = Playing(t);
        if (!t.matchId.empty() && t.phase != netplay::tournament::Phase::Idle) {
            const bool failed = t.phase == netplay::tournament::Phase::Failed;
            rows.push_back(Info("tm-current", loc::T("tournament.current"), PlayState(t),
                failed ? TournamentFailure(t.reason) : std::string(loc::T("tournament.current_detail"))));
            if (playing) rows.push_back(ConfirmRow("tm-stop", loc::T("tournament.stop"), loc::T("tournament.stop_detail")));
        }
        const auto* bridge = FindBridge(v, bridge_);
        if (!bridge) {
            rows.push_back(Info("tm-no-service", loc::T("tournament.needs_service"), {}, loc::T("tournament.needs_service_detail")));
            rows.push_back(Row("linked-accounts", loc::T("screen.linked_accounts"), loc::T("identity.linked_detail")));
            return rows;
        }
        rows.push_back(ServiceRow(v, *bridge));
        if (t.list.bridge == bridge_) {
            const bool free = v.session.room == netplay::RoomState::Idle && !playing;
            for (const auto& match : t.list.items) {
                const bool slotKnown = match.slot == 0 || match.slot == 1;
                const auto own = slotKnown ? match.wins[static_cast<std::size_t>(match.slot)] : 0u;
                const auto other = slotKnown ? match.wins[static_cast<std::size_t>(1 - match.slot)] : 0u;
                std::string detail = loc::Tf("tournament.row_detail",
                    match.opponentFingerprint.empty() ? std::string("?") : match.opponentFingerprint, match.slot + 1);
                detail += "\n\n";
                const bool playable = match.PlayedInEmber() && !match.Finished();
                detail += match.Finished() ? loc::T("tournament.finished_detail") : !match.PlayedInEmber() ? loc::T("tournament.organizer_detail") :
                    free ? loc::T("tournament.play_detail") : loc::T("tournament.leave_room_detail");
                auto row = Row("tm-play:" + match.matchId, match.roundLabel.empty() ? std::string(loc::T("tournament.match")) : match.roundLabel,
                    std::move(detail), playable && free);
                row.value = loc::Tf("tournament.score", own, other, match.gamesToWin);
                row.userText = !match.roundLabel.empty();
                row.hint = loc::T("tournament.play");
                if (!playable) row.info = true;
                rows.push_back(std::move(row));
            }
            if (t.list.items.empty() && !t.list.loading && t.list.error.empty())
                rows.push_back(Info("tm-none", loc::T("tournament.none"), {}, loc::T("tournament.none_detail")));
        }
        rows.push_back(Row("tm-refresh", loc::T("identity.refresh"), loc::T("tournament.refresh_detail"), !t.list.loading));
        rows.push_back(Row("tm-paste", loc::T("tournament.paste_link"), loc::T("tournament.paste_link_detail")));
        rows.back().hint = loc::T("menu.hint.paste");
    }
    return rows;
}

void IdentityPanel::Activate(const MenuAction& a, const ShellView& v, MenuNavigation& nav) {
    const auto& id = v.identity;
    IdentityRequest r;
    if (a.id == "linked-accounts" || a.id == "identity-backup" || a.id == "tournament-matches") { nav.Push(a.id); return; }
    if (a.id == "tm-refresh") { wantAssignments_ = true; message_.clear(); return; }
    if (a.id == "tm-paste") {
        // The match link the tournament site gave: its page or the ember: link.
        const char* text = ImGui::GetClipboardText();
        const auto link = tournament_link::ParsePasted(text ? text : "");
        if (!link.Valid()) { Say(loc::T("tournament.failure.link"), true); return; }
        message_.clear();
        OpenMatch(link.bridgeId, link.matchId);
        return;
    }
    if (a.id == "tm-stop") { play_ = netplay::tournament::Command{netplay::tournament::Command::Op::Stop, {}, {}}; return; }
    if (a.id.compare(0, 8, "tm-play:") == 0) {
        play_ = netplay::tournament::Command{netplay::tournament::Command::Op::Play, bridge_, a.id.substr(8)};
        message_.clear();
        return;
    }
    if (a.id == "id-copy") {
        if (!id.emberId.empty()) { ImGui::SetClipboardText(id.emberId.c_str()); Say(loc::T("identity.done.copied"), false, 3); }
        return;
    }
    if (a.id == "id-restore-paste") {
        const char* text = ImGui::GetClipboardText();
        std::string path = text ? text : "";
        // Explorer's "Copy as path" wraps the path in quotes.
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
        if (path.empty() || path.size() > IdentityRequest::MaxPath) Say(loc::T("identity.failure.paste"), true);
        else restorePath_ = path;
        return;
    }
    if (a.id == "id-enable") {
        r.op = IdentityOp::Enable;
        if (id.passphraseRequired) r.passphrase = newPassphrase_;
    } else if (a.id == "id-reset") { r.op = IdentityOp::Reset; r.emberId = id.emberId; }
    else if (a.id == "id-export") { r.op = IdentityOp::Export; r.passphrase = backupPassphrase_; }
    else if (a.id == "id-preview") { r.op = IdentityOp::PreviewImport; r.path = restorePath_; r.passphrase = restorePassphrase_; }
    else if (a.id == "id-import") {
        r.op = IdentityOp::Import; r.path = restorePath_; r.passphrase = restorePassphrase_;
        r.emberId = id.previewEmberId; r.replace = id.previewReplaces;
        if (id.passphraseRequired) r.localPassphrase = newPassphrase_;
    } else if (a.id == "id-claim") { r.op = IdentityOp::LinkClaim; r.bridge = bridge_; r.connection = connection_; r.code = code_; }
    else if (a.id == "id-refresh") { Refresh(v, "linked-accounts"); return; }
    else if (a.id == "id-forget") { r.op = IdentityOp::BridgeForget; r.bridge = bridge_; }
    else if (a.id == "id-lookup") { r.op = IdentityOp::BridgeInspect; r.origin = origin_; lookingUp_ = true; found_ = {}; }
    else if (a.id == "id-approve") { r.op = IdentityOp::BridgeApprove; r.origin = found_.origin; r.bridge = found_.id; }
    else if (a.id.compare(0, 10, "id-unlink:") == 0) { r.op = IdentityOp::LinkRemove; r.bridge = bridge_; r.target = a.id.substr(10); }
    else if (a.id.compare(0, 10, "id-cancel:") == 0) { r.op = IdentityOp::LinkCancel; r.bridge = bridge_; r.target = a.id.substr(10); }
    else return;
    message_.clear();
    Queue(std::move(r));
}

void IdentityPanel::Accept(const MenuAction& a, const ShellView& v) {
    if (a.id == "id-new-passphrase") Assign(newPassphrase_, a.text);
    else if (a.id == "id-new-confirm") Assign(newConfirm_, a.text);
    else if (a.id == "id-backup-passphrase") Assign(backupPassphrase_, a.text);
    else if (a.id == "id-backup-confirm") Assign(backupConfirm_, a.text);
    else if (a.id == "id-restore-passphrase") Assign(restorePassphrase_, a.text);
    else if (a.id == "id-restore-path") restorePath_ = a.text;
    else if (a.id == "id-origin") origin_ = a.text;
    else if (a.id == "id-code") code_ = a.text;
    else if (a.id == "id-unlock" && !a.text.empty()) {
        IdentityRequest r; r.op = IdentityOp::Unlock; r.passphrase = a.text;
        message_.clear(); Queue(std::move(r));
    } else if (a.id == "id-bridge" && a.kind == MenuAction::Chosen) { SelectBridge(v, a.text); wantAssignments_ = true; }
    else if (a.id == "id-connection" && a.kind == MenuAction::Chosen) connection_ = a.text;
}
} }
