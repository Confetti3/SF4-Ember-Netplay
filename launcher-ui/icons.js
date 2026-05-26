(function () {
  function createIcon(name, className) {
    var svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("class", className || "icon");
    svg.setAttribute("aria-hidden", "true");
    var use = document.createElementNS("http://www.w3.org/2000/svg", "use");
    use.setAttribute("href", "#i-" + name);
    svg.appendChild(use);
    return svg;
  }

  function messageWithIcon(text, iconName) {
    var wrap = document.createElement("span");
    wrap.className = "message-with-icon";
    if (iconName) wrap.appendChild(createIcon(iconName));
    wrap.appendChild(document.createTextNode(text));
    return wrap;
  }

  window.SF4eIcons = {
    create: createIcon,
    message: messageWithIcon,
  };
})();
