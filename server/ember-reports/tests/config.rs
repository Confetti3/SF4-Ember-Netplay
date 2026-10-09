use ember_reports::config::Config;

#[test]
fn config_rejects_public_bind_and_remote_or_credentialled_bugsink() {
    let defaults = || Config {
        state_dir: std::env::temp_dir().join("ember-reports"),
        ..Config::default()
    };
    let mut config = defaults();
    assert!(config.validate().is_ok());
    for address in ["0.0.0.0:47850", "[::1]:47850", "127.0.0.2:47850"] {
        config.bind = address.parse().unwrap();
        assert!(config.validate().is_err());
    }
    config = defaults();
    for url in [
        "https://127.0.0.1:47860",
        "http://example.com",
        "http://key@127.0.0.1:47860",
        "http://127.0.0.1:47860/other",
    ] {
        config.bugsink_base = url.into();
        assert!(config.validate().is_err());
    }
    config = defaults();
    config.limits.workers = 5;
    assert!(config.validate().is_err());
    config = defaults();
    config.limits.queue = 33;
    assert!(config.validate().is_err());
}

#[test]
fn config_rejects_a_bind_port_nginx_does_not_proxy_to() {
    let mut config = Config {
        state_dir: std::env::temp_dir().join("ember-reports"),
        ..Config::default()
    };
    for port in [0u16, 1, 47851, 47860] {
        config.bind = format!("127.0.0.1:{port}").parse().unwrap();
        assert!(config.validate().is_err(), "{port}");
    }
    config.bind = format!("127.0.0.1:{}", ember_reports::config::BIND_PORT)
        .parse()
        .unwrap();
    assert!(config.validate().is_ok());
}

fn deploy_file(path: &str) -> String {
    std::fs::read_to_string(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join(path)).unwrap()
}

#[test]
fn nginx_proxies_to_the_validated_port_with_headers_on_every_response() {
    let locations = deploy_file("deploy/nginx/ember-reports-locations.conf");
    let upstream = format!(
        "proxy_pass http://127.0.0.1:{};",
        ember_reports::config::BIND_PORT
    );
    assert!(
        locations.contains(&upstream),
        "proxy_pass must match BIND_PORT"
    );
    assert!(
        deploy_file("deploy/config.example.json")
            .contains(&format!("127.0.0.1:{}", ember_reports::config::BIND_PORT))
    );
    // Each location that sets its own add_header (the proxy and the 429
    // named location) must include the common headers, or nginx drops the
    // server's.
    let blocks: Vec<&str> = locations.split("\nlocation ").skip(1).collect();
    assert_eq!(blocks.len(), 2);
    for block in blocks {
        assert!(block.contains("add_header"));
        assert!(
            block.contains("include snippets/ember-reports-headers.conf;"),
            "{block}"
        );
    }
    let ours = deploy_file("deploy/nginx/ember-reports-headers.conf");
    let shared = deploy_file("../ember-short/deploy/nginx/ember-short-headers.conf");
    let directives = |text: &str| -> Vec<String> {
        text.lines()
            .filter(|line| line.starts_with("add_header"))
            .map(str::to_owned)
            .collect()
    };
    assert_eq!(directives(&ours).len(), 3);
    assert_eq!(directives(&ours), directives(&shared));
    let setup = deploy_file("deploy/setup.sh");
    assert!(setup.contains("nginx/ember-reports-headers.conf"));
    assert!(setup.contains(r#""$LOCATIONS" "$HEADERS""#));
}
