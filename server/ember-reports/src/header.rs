use crate::intake::Report;

/// The first line of an attached `launcher.log` can be a report header, which
/// a client writes to say what kind of ending the report is about:
///
/// `report: kind=start_failure phase=before_signal hint=steam_appid_missing game_log=earlier_session`
///
/// Fields are `key=value` separated by single spaces; a value is a short code
/// of `a-z`, `0-9` and `_` (`hint` may list several, comma separated). Unknown
/// keys and invalid values are ignored. The report's own metadata stays the
/// original schema 1, so a service that does not read the header stores the
/// log as it is.
#[derive(Default)]
pub struct Header {
    /// `kind=start_failure`: the game ended during its own start-up.
    pub start_failure: bool,
    /// `before_signal` or `after_signal`: relative to the game's DLL
    /// signalling the launcher.
    pub phase: Option<String>,
    /// What the launcher knew about why, such as `steam_appid_missing`.
    pub hint: Option<String>,
    /// `this_session`, `earlier_session` (sf4e.log left out) or `absent`.
    pub game_log: Option<String>,
}

const CAP: usize = 80;

fn code(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= CAP
        && value
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_' || b == b',')
}

pub fn parse(report: &Report) -> Header {
    let mut header = Header::default();
    let Some(line) = report
        .logs
        .get("launcher.log")
        .and_then(|log| log.lines().next())
        .and_then(|line| line.strip_prefix("report: "))
    else {
        return header;
    };
    for field in line.split(' ') {
        let Some((key, value)) = field.split_once('=') else {
            continue;
        };
        if !code(value) {
            continue;
        }
        match key {
            "kind" => header.start_failure = value == "start_failure",
            "phase" => header.phase = Some(value.to_owned()),
            "hint" => header.hint = Some(value.to_owned()),
            "game_log" => header.game_log = Some(value.to_owned()),
            _ => {}
        }
    }
    header
}
