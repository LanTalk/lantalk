package main

import "fyne.io/fyne/v2"

var resourceAppIcon = fyne.NewStaticResource("app_icon.svg", []byte(`
<svg width="256" height="256" viewBox="0 0 256 256" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="g1" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%" stop-color="#2E8CFF"/>
      <stop offset="100%" stop-color="#1E5ED8"/>
    </linearGradient>
  </defs>
  <rect x="16" y="16" width="224" height="224" rx="52" fill="url(#g1)"/>
  <path d="M64 86c0-13.3 10.7-24 24-24h80c13.3 0 24 10.7 24 24v54c0 13.3-10.7 24-24 24h-44l-28 24c-4.6 3.9-11.6 0.7-11.6-5.3V164H88c-13.3 0-24-10.7-24-24V86z" fill="#FFFFFF"/>
  <circle cx="102" cy="113" r="10" fill="#2E8CFF"/>
  <circle cx="128" cy="113" r="10" fill="#2E8CFF"/>
  <circle cx="154" cy="113" r="10" fill="#2E8CFF"/>
</svg>
`))

var resourceDefaultAvatar = fyne.NewStaticResource("default_avatar.svg", []byte(`
<svg width="256" height="256" viewBox="0 0 256 256" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="g2" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%" stop-color="#89B5FF"/>
      <stop offset="100%" stop-color="#5C8FF2"/>
    </linearGradient>
  </defs>
  <circle cx="128" cy="128" r="120" fill="#EAF2FF"/>
  <circle cx="128" cy="98" r="44" fill="url(#g2)"/>
  <path d="M44 216c14-38 46-60 84-60s70 22 84 60" fill="url(#g2)"/>
</svg>
`))
