import { ButtonItem, Field, PanelSection, PanelSectionRow, staticClasses } from "@decky/ui";
import { callable, definePlugin, toaster } from "@decky/api";
import { useEffect, useState } from "react";
import { FaFistRaised } from "react-icons/fa";

const USF4 = 45760;

type Status = { installed: string; latest: string; option: string };
const getStatus = callable<[], Status>("status");
const install = callable<[], { ok: boolean; message: string }>("install");

// Resolves empty when Steam has no details for the game, such as when it is not owned.
function launchOptions(): Promise<string> {
  return new Promise((resolve) => {
    let reg: { unregister(): void } | undefined;
    const done = (options: string) => {
      setTimeout(() => reg?.unregister());
      resolve(options);
    };
    reg = SteamClient.Apps.RegisterForAppDetails(USF4, (details) => done(details.strLaunchOptions));
    setTimeout(() => done(""), 3000);
  });
}

function Content() {
  const [status, setStatus] = useState<Status>();
  const [option, setOption] = useState<string>();
  const [busy, setBusy] = useState(false);

  const refresh = () => {
    getStatus().then(setStatus, () => setStatus({ installed: "", latest: "", option: "" }));
    launchOptions().then(setOption);
  };
  useEffect(refresh, []);

  const run = async () => {
    setBusy(true);
    try {
      const result = await install();
      if (result.ok && status?.option) SteamClient.Apps.SetAppLaunchOptions(USF4, status.option);
      toaster.toast({ title: result.ok ? "Ember installed" : "Ember install failed", body: result.message });
    } catch (e) {
      toaster.toast({ title: "Ember install failed", body: String(e) });
    } finally {
      setBusy(false);
      refresh();
    }
  };

  return (
    <PanelSection>
      <PanelSectionRow>
        <Field label="Installed">{status ? status.installed || "No" : "..."}</Field>
      </PanelSectionRow>
      <PanelSectionRow>
        <Field label="Latest">{status ? status.latest || "Unknown" : "..."}</Field>
      </PanelSectionRow>
      <PanelSectionRow>
        <Field label="Launch option">{option === undefined ? "..." : option === status?.option ? "Set" : "Not set"}</Field>
      </PanelSectionRow>
      <PanelSectionRow>
        <ButtonItem layout="below" disabled={busy || !status} onClick={run}>
          {busy ? "Installing..." : status?.installed ? "Update Ember" : "Install Ember"}
        </ButtonItem>
      </PanelSectionRow>
    </PanelSection>
  );
}

export default definePlugin(() => ({
  name: "Ember",
  titleView: <div className={staticClasses.Title}>Ember</div>,
  content: <Content />,
  icon: <FaFistRaised />,
}));
