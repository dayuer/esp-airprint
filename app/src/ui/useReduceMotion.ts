import {useEffect, useState} from 'react';
import {AccessibilityInfo} from 'react-native';

/**
 * 系统的「减弱动态效果」。开着时断点不呼吸，改成静态强色——
 * 告警不能因为无障碍设置就消失，只能换一种表达。
 */
export function useReduceMotion(): boolean {
  const [reduce, setReduce] = useState(false);
  useEffect(() => {
    let alive = true;
    AccessibilityInfo.isReduceMotionEnabled().then(v => alive && setReduce(v));
    const sub = AccessibilityInfo.addEventListener('reduceMotionChanged', setReduce);
    return () => {
      alive = false;
      sub.remove();
    };
  }, []);
  return reduce;
}
