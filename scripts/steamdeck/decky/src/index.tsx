import { ButtonItem, Field, PanelSection, PanelSectionRow, staticClasses } from "@decky/ui";
import { callable, definePlugin, toaster } from "@decky/api";
import { useEffect, useState } from "react";
import { FaFistRaised } from "react-icons/fa";

type Status = { installed: string; latest: string };
const getStatus = callable<[], Status>("status");
const install = callable<[], { ok: boolean; message: string }>("install");

function Content() {
  const [status, setStatus] = useState<Status>();
  const [busy, setBusy] = useState(false);

  const refresh = () => {
    getStatus().then(setStatus, () => setStatus({ installed: "", latest: "" }));
  };
  useEffect(refresh, []);

  // On success Steam restarts to show the SF4 Ember Netplay tile, so this
  // panel usually closes before the toast; a failure leaves Steam running.
  const run = async () => {
    setBusy(true);
    try {
      const result = await install();
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
        <ButtonItem layout="below" disabled={busy || !status} onClick={run}
          description="Steam restarts to add SF4 Ember Netplay to your library.">
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
