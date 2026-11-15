import type { SVGProps } from 'react';

function IconBase({ children, ...props }: SVGProps<SVGSVGElement>) {
  return (
    <svg
      width="16"
      height="16"
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.5"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      {...props}
    >
      {children}
    </svg>
  );
}

export function PlusIcon() {
  return (
    <IconBase>
      <path d="M8 3v10M3 8h10" />
    </IconBase>
  );
}

export function LinkIcon() {
  return (
    <IconBase>
      <path d="M6.5 9.5l3-3" />
      <path d="M7.5 4.5l1.2-1.2a3 3 0 1 1 4.2 4.2l-1.2 1.2" />
      <path d="M8.5 11.5l-1.2 1.2a3 3 0 1 1-4.2-4.2l1.2-1.2" />
    </IconBase>
  );
}

export function TrashIcon() {
  return (
    <IconBase>
      <path d="M2.5 4h11M5.5 4V2.8h5V4" />
      <path d="M4 4l.7 9h6.6L12 4" />
      <path d="M6.5 6.5v4M9.5 6.5v4" />
    </IconBase>
  );
}

export function CopyIcon() {
  return (
    <IconBase>
      <rect x="5" y="5" width="8" height="8" rx="1" />
      <path d="M11 3H4a1 1 0 0 0-1 1v7" />
    </IconBase>
  );
}

export function PasteIcon() {
  return (
    <IconBase>
      <path d="M6 3H4a1 1 0 0 0-1 1v9a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V4a1 1 0 0 0-1-1h-2" />
      <rect x="6" y="1.8" width="4" height="2.4" rx="0.8" />
      <path d="M5.5 8h5M5.5 10.5h3.5" />
    </IconBase>
  );
}

export function LayersIcon() {
  return (
    <IconBase>
      <path d="M8 2.5L2.5 5.5 8 8.5l5.5-3L8 2.5z" />
      <path d="M2.5 8.2L8 11.2l5.5-3" />
      <path d="M2.5 10.8L8 13.8l5.5-3" />
    </IconBase>
  );
}

export function SaveIcon() {
  return (
    <IconBase>
      <path d="M3.5 2.5h7L13 5v8.5H3.5z" />
      <path d="M5.5 2.5v4h5v-4M5.5 13.5v-4h5v4" />
    </IconBase>
  );
}

export function FolderOpenIcon() {
  return (
    <IconBase>
      <path d="M2 4.5h4l1.2 1.5H14l-1.8 7H3.8z" />
      <path d="M2 4.5v8h1.8" />
    </IconBase>
  );
}

export function XIcon() {
  return (
    <IconBase>
      <path d="M4 4l8 8M12 4l-8 8" />
    </IconBase>
  );
}
