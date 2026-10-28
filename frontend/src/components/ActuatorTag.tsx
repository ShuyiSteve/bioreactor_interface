interface ActuatorTagProps {
  label: string
  on: boolean
  kind: string
}

export default function ActuatorTag({ label, on, kind }: ActuatorTagProps) {
  return (
    <span className={`actuator-tag ${kind} ${on ? 'on' : 'off'}`}>
      <i className="actuator-dot" />
      {label}
    </span>
  )
}
