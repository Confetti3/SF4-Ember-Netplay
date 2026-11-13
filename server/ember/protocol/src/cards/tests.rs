use serde_json::{Value, json};

use super::*;
use crate::levels::tests::{ignores_extra, read, refuses_extra};
use crate::awards::MAX_FAMILY_ID;

const FRIEND: &str = "fr_11111111-1111-4111-8111-111111111111";
const REQUEST: &str = "rq_11111111-1111-4111-8111-111111111111";

#[test]
fn card_tokens_are_a_prefix_and_up_to_96_base64url_characters() {
    let seventy_six = format!("c1.{}", "aZ09-_".repeat(13).get(..76).unwrap());
    assert!(is_card_token(&seventy_six));
    assert!(is_card_token("c1.a"));
    assert!(is_card_token(&format!("c1.{}", "A".repeat(MAX_CARD_BODY))));
    for bad in [
        "",
        "c1.",
        "c1",
        "c2.abc",
        "C1.abc",
        "abc",
        "c1.a b",
        "c1.a=",
        "c1.a+b",
        "c1.a/b",
        "c1.\u{e9}",
        "c1.a\n",
    ] {
        assert!(!is_card_token(bad), "{bad:?}");
    }
    assert!(!is_card_token(&format!(
        "c1.{}",
        "A".repeat(MAX_CARD_BODY + 1)
    )));
}

#[test]
fn a_look_up_takes_one_to_sixteen_tokens_or_friend_ids() {
    let wire = json!({"cards": [{"card": "c1.abc"}, {"friend": FRIEND}]});
    let lookup: CardLookup = read(&wire);
    assert_eq!(
        lookup.cards,
        vec![
            CardSelector::Card("c1.abc".into()),
            CardSelector::Friend(FRIEND.into())
        ]
    );
    assert!(lookup.check().is_ok());
    assert_eq!(serde_json::to_value(&lookup).unwrap(), wire);

    let cards = |count: usize| CardLookup {
        cards: vec![CardSelector::Card("c1.abc".into()); count],
    };
    assert!(cards(1).check().is_ok());
    assert!(cards(MAX_LOOKUP).check().is_ok());
    assert_eq!(cards(0).check(), Err(Error::InvalidField("cards")));
    assert_eq!(
        cards(MAX_LOOKUP + 1).check(),
        Err(Error::InvalidField("cards"))
    );
    assert_eq!(
        CardSelector::Card("fr_x".into()).check(),
        Err(Error::InvalidField("card"))
    );
    for friend in ["c1.abc", "rq_11111111-1111-4111-8111-111111111111", "fr_x"] {
        assert_eq!(
            CardSelector::Friend(friend.into()).check(),
            Err(Error::InvalidField("friend")),
            "{friend}"
        );
    }

    // A request the caller received and a friend code name a player too.
    let more = json!({"cards": [{"request": REQUEST}, {"code": "4f2a 91cx"}]});
    let lookup: CardLookup = read(&more);
    assert_eq!(
        lookup.cards,
        vec![
            CardSelector::Request(REQUEST.into()),
            CardSelector::Code("4f2a 91cx".into())
        ]
    );
    assert!(lookup.check().is_ok());
    assert_eq!(serde_json::to_value(&lookup).unwrap(), more);
    for (selector, field) in [
        (CardSelector::Request(FRIEND.into()), "request"),
        (CardSelector::Request("rq_x".into()), "request"),
        (CardSelector::Code("4F2A-91C".into()), "code"),
        (CardSelector::Code("4F2A-91CXX".into()), "code"),
        (CardSelector::Code("4F2A-91C!".into()), "code"),
        (CardSelector::Code(String::new()), "code"),
    ] {
        assert_eq!(selector.check(), Err(Error::InvalidField(field)), "{selector:?}");
    }

    // A selector names one thing, and nothing it does not know.
    refuses_extra::<CardLookup>(&wire, "");
    for bad in [
        json!({"cards": [{"card": "c1.abc", "friend": FRIEND}]}),
        json!({"cards": [{"card": "c1.abc", "extra": 1}]}),
        json!({"cards": [{"name": "c1.abc"}]}),
        json!({"cards": [{}]}),
        json!({"cards": [{"card": 1}]}),
    ] {
        assert!(
            serde_json::from_value::<CardLookup>(bad.clone()).is_err(),
            "{bad}"
        );
    }
}

#[test]
fn a_look_up_answer_has_one_entry_per_selector() {
    let wire = json!({"cards": [
        {"account": {"points": 2100, "tier": 2},
         "fighters": [{"fighter": 3, "points": 1986, "tier": 3}], "relation": "none"},
        null]});
    let answers: CardAnswers = read(&wire);
    assert!(answers.check().is_ok());
    assert_eq!(serde_json::to_value(&answers).unwrap(), wire);
    assert_eq!(answers.cards[1], None);

    let request = CardLookup {
        cards: vec![
            CardSelector::Card("c1.abc".into()),
            CardSelector::Friend(FRIEND.into()),
        ],
    };
    assert!(answers.check_for(&request).is_ok());
    let short = CardLookup {
        cards: request.cards[..1].to_vec(),
    };
    assert_eq!(answers.check_for(&short), Err(Error::Mismatch("cards")));

    // A card with nothing played still carries its account and an empty
    // list of fighters.
    let empty = PlayerCard::bare(CardAccount { points: 0, tier: 0 }, Relation::Sent);
    assert_eq!(
        serde_json::to_string(&empty).unwrap(),
        r#"{"account":{"points":0,"tier":0},"fighters":[],"relation":"sent"}"#
    );
    assert!(empty.check().is_ok());
    // A reader without `fighters` reads none; one without `account`
    // refuses the card.
    let bare: PlayerCard =
        read(&json!({"account": {"points": 2, "tier": 0}, "relation": "none"}));
    assert!(bare.fighters.is_empty());
    assert!(
        serde_json::from_value::<PlayerCard>(json!({"fighters": [], "relation": "none"}))
            .is_err()
    );

    let empty_answer = CardAnswers { cards: vec![] };
    assert_eq!(empty_answer.check(), Err(Error::InvalidField("cards")));
    let long = CardAnswers {
        cards: vec![None; MAX_LOOKUP + 1],
    };
    assert_eq!(long.check(), Err(Error::InvalidField("cards")));

    ignores_extra::<CardAnswers>(&wire, "");
    ignores_extra::<CardAnswers>(&wire, "/cards/0");
    ignores_extra::<CardAnswers>(&wire, "/cards/0/account");
    ignores_extra::<CardAnswers>(&wire, "/cards/0/fighters/0");
    // Wins and games are not on another player's card: the bridge never
    // sends them, and a reader that is sent them keeps none.
    for member in ["wins", "games"] {
        let mut with_member = wire.clone();
        with_member["cards"][0]["fighters"][0][member] = json!(3);
        assert_eq!(
            read::<CardAnswers>(&with_member),
            read::<CardAnswers>(&wire),
            "{member}"
        );
    }
}

#[test]
fn a_card_fighter_is_checked_and_listed_in_order() {
    let fighter = |fighter: u8, points: u64, tier: u8| CardFighter {
        fighter,
        points,
        tier,
    };
    let card = |fighters: Vec<CardFighter>| PlayerCard {
        fighters,
        ..PlayerCard::bare(CardAccount { points: 1, tier: 0 }, Relation::None)
    };
    assert!(
        card(vec![fighter(0, 1, 0), fighter(43, 90_000, 4)])
            .check()
            .is_ok()
    );
    for bad in [fighter(44, 1, 0), fighter(3, 0, 0), fighter(3, 1, 5)] {
        assert!(bad.check().is_err(), "{bad:?}");
        assert!(card(vec![bad]).check().is_err(), "{bad:?}");
    }
    assert_eq!(
        PlayerCard::bare(CardAccount { points: 1, tier: 5 }, Relation::None).check(),
        Err(Error::InvalidField("tier"))
    );
    for unordered in [
        vec![fighter(5, 1, 0), fighter(4, 1, 0)],
        vec![fighter(5, 1, 0), fighter(5, 2, 0)],
    ] {
        assert_eq!(
            card(unordered).check(),
            Err(Error::InvalidField("fighters"))
        );
    }
}

#[test]
fn relations_use_the_documents_names() {
    for (relation, name) in [
        (Relation::None, "none"),
        (Relation::Friend, "friend"),
        (Relation::Sent, "sent"),
        (Relation::Received, "received"),
        (Relation::Blocked, "blocked"),
    ] {
        assert_eq!(relation.as_str(), name);
        assert_eq!(Relation::parse(name), Some(relation));
        assert_eq!(serde_json::to_value(relation).unwrap(), name);
    }
    assert_eq!(Relation::ALL.len(), 5);
    // A relation a later bridge adds reads as none.
    assert_eq!(
        serde_json::from_value::<Relation>(json!("blocked_by")).unwrap(),
        Relation::None
    );
}

fn full_card() -> Value {
    json!({"account": {"points": 2100, "tier": 2},
        "fighters": [{"fighter": 3, "points": 1986, "tier": 3}], "relation": "friend",
        "awards": {"games": 1200, "wins": 700, "best_streak": 9, "peak_class": 7},
        "name": "Player 2", "country": "FR", "main": 5,
        "badge": {"band": 7, "settled": true}, "since": 1748736000,
        "worn": {"title": "matches", "icon": "streak"}, "pins": ["rank", "wins"],
        "style": {"device": 2, "link": 1, "region": "euc1", "looking": 34},
        "versus": {"won": 4, "lost": 3, "last_at": 1731000000}, "friend": FRIEND})
}

#[test]
fn a_card_says_what_its_player_shows_and_only_what_there_is() {
    let wire = full_card();
    let card: PlayerCard = read(&wire);
    assert!(card.check().is_ok());
    assert_eq!(card.name.as_deref(), Some("Player 2"));
    assert_eq!(card.main, Some(5));
    assert_eq!(card.since, Some(1_748_736_000));
    assert_eq!(card.worn.title.as_deref(), Some("matches"));
    assert_eq!(card.pins, ["rank", "wins"]);
    assert_eq!(card.style.device, 2);
    assert_eq!(card.versus, Some(PairRecord { won: 4, lost: 3, last_at: 1_731_000_000 }));
    assert_eq!(card.friend.as_deref(), Some(FRIEND));
    assert_eq!(serde_json::to_value(&card).unwrap(), wire);
    ignores_extra::<PlayerCard>(&wire, "");
    ignores_extra::<PlayerCard>(&wire, "/style");
    ignores_extra::<PlayerCard>(&wire, "/worn");
    ignores_extra::<PlayerCard>(&wire, "/versus");

    // Nothing to say, nothing written: not a name, a main, an empty
    // worn, no pins, a style of zeros, a record before a first game.
    let plain = PlayerCard::bare(CardAccount { points: 0, tier: 0 }, Relation::None);
    assert_eq!(
        serde_json::to_string(&plain).unwrap(),
        r#"{"account":{"points":0,"tier":0},"fighters":[],"relation":"none"}"#
    );
    let zeros = PlayerCard {
        style: Style { device: 0, link: 0, region: None, looking: 0 },
        worn: Worn { title: None, icon: None },
        ..plain.clone()
    };
    assert_eq!(zeros, plain);
    // A style with only a region says only that, and a worn title alone.
    let some = PlayerCard {
        style: Style { region: Some("use1".into()), ..Style::default() },
        worn: Worn { title: Some("rank".into()), icon: None },
        ..plain
    };
    let wire = serde_json::to_value(&some).unwrap();
    assert_eq!(wire["style"], json!({"region": "use1"}));
    assert_eq!(wire["worn"], json!({"title": "rank"}));
}

#[test]
fn a_card_with_a_value_it_cannot_hold_is_refused() {
    let good: PlayerCard = read(&full_card());
    let broken = [
        ("name", PlayerCard { name: Some(String::new()), ..good.clone() }),
        ("name", PlayerCard { name: Some("a\nb".into()), ..good.clone() }),
        ("country", PlayerCard { country: Some("fr".into()), ..good.clone() }),
        ("main", PlayerCard { main: Some(FIGHTERS), ..good.clone() }),
        ("family", PlayerCard {
            worn: Worn { title: Some("Rank".into()), icon: None },
            ..good.clone()
        }),
        ("pins", PlayerCard {
            pins: ["a", "b", "c", "d", "e"].map(String::from).to_vec(),
            ..good.clone()
        }),
        ("pins", PlayerCard { pins: vec!["x y".into()], ..good.clone() }),
        ("region", PlayerCard {
            style: Style { region: Some("EUC1".into()), ..Style::default() },
            ..good.clone()
        }),
        ("record", PlayerCard {
            versus: Some(PairRecord { won: 0, lost: 0, last_at: 1 }),
            ..good.clone()
        }),
        ("friend", PlayerCard { friend: Some("rq_x".into()), ..good.clone() }),
        ("badge", PlayerCard {
            badge: Some(SkillBadge::class(11, true)),
            ..good.clone()
        }),
    ];
    for (field, card) in broken {
        assert_eq!(card.check(), Err(Error::InvalidField(field)), "{card:?}");
    }
    // A device, link or bit a later bridge adds is read, as not shown.
    let later = PlayerCard {
        style: Style { device: 9, link: 7, region: Some("sae1".into()), looking: 255 },
        ..good
    };
    assert!(later.check().is_ok());
    let wire = serde_json::to_value(&later).unwrap();
    assert_eq!(read::<PlayerCard>(&wire), later);
}

fn save() -> CardSave {
    read(&json!({"listing": {"name": "Player 1", "country": "DE"}, "main": 3,
        "worn": {"title": "matches", "icon": "streak"}, "pins": ["rank", "wins"],
        "style": {"device": 2, "link": 1, "region": "euc1", "looking": 34}}))
}

#[test]
fn a_card_save_is_checked_before_it_is_sent_or_stored() {
    let good = save();
    assert!(good.check().is_ok());
    assert_eq!(
        serde_json::to_value(&good).unwrap(),
        json!({"listing": {"name": "Player 1", "country": "DE"}, "main": 3,
            "worn": {"title": "matches", "icon": "streak"}, "pins": ["rank", "wins"],
            "style": {"device": 2, "link": 1, "region": "euc1", "looking": 34}})
    );
    // Only the listing is needed; the rest says nothing.
    let bare: CardSave = read(&json!({"listing": {"name": "Player 1"}}));
    assert!(bare.check().is_ok());
    assert_eq!(
        serde_json::to_value(&bare).unwrap(),
        json!({"listing": {"name": "Player 1"}})
    );
    // Four pins, two bits, the last device and link, every known region.
    for region in crate::relay::REGIONS {
        let mut each = good.clone();
        each.style.region = Some(region.into());
        assert!(each.check().is_ok(), "{region}");
    }
    let mut edge = good.clone();
    edge.pins = ["a", "b", "c", "d"].map(String::from).to_vec();
    edge.style = Style { device: 4, link: 2, region: None, looking: 0b10_0001 };
    edge.main = Some(FIGHTERS - 1);
    assert!(edge.check().is_ok());

    let with = |change: &dyn Fn(&mut CardSave)| {
        let mut card = good.clone();
        change(&mut card);
        card.check()
    };
    assert_eq!(
        with(&|card| card.pins = ["a", "b", "c", "d", "e"].map(String::from).to_vec()),
        Err(Error::InvalidField("pins"))
    );
    assert_eq!(
        with(&|card| card.pins = vec!["rank".into(), "rank".into()]),
        Err(Error::InvalidField("pins"))
    );
    assert_eq!(
        with(&|card| card.pins = vec!["Rank".into()]),
        Err(Error::InvalidField("pins"))
    );
    assert_eq!(
        with(&|card| card.pins = vec![String::new()]),
        Err(Error::InvalidField("pins"))
    );
    assert_eq!(
        with(&|card| card.pins = vec!["a".repeat(MAX_FAMILY_ID + 1)]),
        Err(Error::InvalidField("pins"))
    );
    assert!(with(&|card| card.pins = vec!["a".repeat(MAX_FAMILY_ID)]).is_ok());
    assert_eq!(
        with(&|card| card.worn.icon = Some("no_underscore".into())),
        Err(Error::InvalidField("family"))
    );
    assert_eq!(
        with(&|card| card.style.looking = 0b111),
        Err(Error::InvalidField("looking"))
    );
    assert_eq!(
        with(&|card| card.style.looking = 64),
        Err(Error::InvalidField("looking"))
    );
    assert_eq!(
        with(&|card| card.style.device = 5),
        Err(Error::InvalidField("device"))
    );
    assert_eq!(
        with(&|card| card.style.link = 3),
        Err(Error::InvalidField("link"))
    );
    assert_eq!(
        with(&|card| card.style.region = Some("mars".into())),
        Err(Error::InvalidField("region"))
    );
    assert_eq!(
        with(&|card| card.main = Some(FIGHTERS)),
        Err(Error::InvalidField("main"))
    );
    assert_eq!(
        with(&|card| card.listing.name = String::new()),
        Err(Error::InvalidField("name"))
    );
    assert_eq!(
        with(&|card| card.listing.country = Some("de".into())),
        Err(Error::InvalidField("country"))
    );
    // A save names nothing it does not know.
    let wire = serde_json::to_value(&good).unwrap();
    refuses_extra::<CardSave>(&wire, "");
    refuses_extra::<CardSave>(&wire, "/listing");
}

#[test]
fn a_search_asks_for_a_name_or_a_code_and_not_both() {
    let name = |text: &str| CardSearch { name: Some(text.into()), code: None };
    let code = |text: &str| CardSearch { name: None, code: Some(text.into()) };
    for good in [name("Pl"), name("  Play  "), name(&"a".repeat(32)), code("4F2A-91CX"), code("4f2a 91cx")] {
        assert!(good.check().is_ok(), "{good:?}");
    }
    assert_eq!(name("  Play ").trimmed_name(), Some("Play"));
    for (bad, field) in [
        (name("a"), "name"),
        (name("  a  "), "name"),
        (name(""), "name"),
        (name(&"a".repeat(33)), "name"),
        (name("two\nlines"), "name"),
        (code("4F2A"), "code"),
        (code(""), "code"),
        (CardSearch::default(), "search"),
        (
            CardSearch { name: Some("Play".into()), code: Some("4F2A-91CX".into()) },
            "search",
        ),
    ] {
        assert_eq!(bad.check(), Err(Error::InvalidField(field)), "{bad:?}");
    }
    assert_eq!(serde_json::to_value(name("Play")).unwrap(), json!({"name": "Play"}));
    assert_eq!(serde_json::to_value(code("4F2A-91CX")).unwrap(), json!({"code": "4F2A-91CX"}));
    let wire = json!({"name": "Play"});
    refuses_extra::<CardSearch>(&wire, "");
}

#[test]
fn a_search_answer_lists_at_most_twenty_players() {
    let wire = json!({"players": [
        {"card": "c1.abc", "name": "Player 2", "country": "FR", "main": 5,
         "badge": {"band": 7, "settled": true}, "since": 1748736000},
        {"card": "c1.abd", "name": "Player 3", "since": 1748736000}], "more": true});
    let answer: SearchAnswer = read(&wire);
    assert!(answer.check().is_ok());
    assert_eq!(serde_json::to_value(&answer).unwrap(), wire);
    assert!(answer.more);
    ignores_extra::<SearchAnswer>(&wire, "");
    ignores_extra::<SearchAnswer>(&wire, "/players/0");
    // `more` is false when a bridge does not say.
    let short: SearchAnswer = read(&json!({"players": []}));
    assert!(!short.more && short.check().is_ok());

    let hit = answer.players[1].clone();
    let many = |count: usize| SearchAnswer { players: vec![hit.clone(); count], more: false };
    assert!(many(MAX_SEARCH).check().is_ok());
    assert_eq!(many(MAX_SEARCH + 1).check(), Err(Error::InvalidField("players")));
    for (field, bad) in [
        ("card", SearchHit { card: "fr_x".into(), ..hit.clone() }),
        ("name", SearchHit { name: String::new(), ..hit.clone() }),
        ("country", SearchHit { country: Some("x".into()), ..hit.clone() }),
        ("main", SearchHit { main: Some(FIGHTERS), ..hit.clone() }),
        ("badge", SearchHit { badge: Some(SkillBadge::class(11, true)), ..hit.clone() }),
    ] {
        assert_eq!(bad.check(), Err(Error::InvalidField(field)), "{bad:?}");
    }
    // A hit without its card or name is no answer.
    assert!(serde_json::from_value::<SearchAnswer>(
        json!({"players": [{"name": "Player 3", "since": 1}]})
    )
    .is_err());
}
