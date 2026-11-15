import type { ButtonHTMLAttributes, ReactNode } from 'react';

interface IconButtonProps extends Omit<ButtonHTMLAttributes<HTMLButtonElement>, 'children'> {
  /** Accessible name and the tooltip shown on hover. */
  label: string;
  icon: ReactNode;
  variant?: 'default' | 'primary' | 'danger';
}

/** A compact square button used by the queue and preset toolbars. */
export function IconButton({
  label,
  icon,
  variant = 'default',
  className = '',
  ...props
}: IconButtonProps) {
  return (
    <button
      {...props}
      type="button"
      className={`icon-button ${variant} ${className}`.trim()}
      aria-label={label}
      title={label}
    >
      {icon}
    </button>
  );
}
